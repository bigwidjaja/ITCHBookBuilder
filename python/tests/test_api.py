"""Tests for SnapshotIndex and the FastAPI app, on a tiny generated file."""

import pytest

import itchbook
from itchfile import tiny_day

fastapi = pytest.importorskip("fastapi")
from fastapi.testclient import TestClient  # noqa: E402  (needs fastapi and httpx)


@pytest.fixture
def tiny(tmp_path):
    return tiny_day(tmp_path / "tiny.itch")


@pytest.mark.parametrize("interval", [0, 1, 60, 3600])
def test_index_matches_replay(tiny, interval):
    index = itchbook.SnapshotIndex(tiny, ["AAPL", "NOPE"], interval)
    assert index.symbols() == ["AAPL"]
    for at in ["09:00:00", "09:30:00", "09:31:30", "09:32:00", "09:33:00", "23:00:00"]:
        assert index.book("AAPL", at, 5) == itchbook.replay(tiny, "AAPL", at).top(5), at


def test_index_errors(tiny):
    index = itchbook.SnapshotIndex(tiny, ["AAPL"], 60)
    with pytest.raises(KeyError):
        index.book("MSFT", "09:30:00")
    with pytest.raises(ValueError):
        index.book("AAPL", "9:30")
    with pytest.raises(ValueError):
        itchbook.SnapshotIndex(tiny, ["AAPL"], -1)


def test_index_stats(tiny):
    stats = itchbook.SnapshotIndex(tiny, ["AAPL"], 60).stats()
    assert stats["messages"] == 5
    assert stats["by_type"] == {"R": 1, "A": 3, "D": 1}
    assert stats["symbols"] == ["AAPL"]
    assert stats["interval_seconds"] == 60
    assert stats["snapshots"] == 33  # every minute from 09:00 to 09:32


@pytest.fixture
def client(tiny, monkeypatch):
    from itchbook.api import app

    monkeypatch.setenv("ITCHBOOK_FILE", str(tiny))
    monkeypatch.setenv("ITCHBOOK_SYMBOLS", "AAPL")
    monkeypatch.setenv("ITCHBOOK_SNAPSHOT_MINUTES", "1")
    with TestClient(app) as c:  # the with block runs startup, which builds the index
        yield c


def test_book_endpoint(client):
    r = client.get("/book/AAPL", params={"at": "09:32:00", "depth": 1})
    assert r.status_code == 200
    assert r.json() == {
        "symbol": "AAPL",
        "at": "09:32:00",
        "bids": [{"price": 100.5, "shares": 10}],
        "asks": [{"price": 101.0, "shares": 50}],
    }


def test_book_errors(client):
    assert client.get("/book/MSFT", params={"at": "09:32:00"}).status_code == 404
    assert client.get("/book/AAPL", params={"at": "9:32"}).status_code == 400
    assert client.get("/book/AAPL", params={"at": "09:32:00", "depth": 0}).status_code == 422


def test_bbo_endpoint(client):
    r = client.get("/bbo/AAPL", params={"start": "09:29:00", "end": "09:31:00", "interval": "1m"})
    assert r.status_code == 200
    rows = r.json()["rows"]
    assert [row["time"] for row in rows] == [
        "09:29:00.000000000", "09:30:00.000000000", "09:31:00.000000000"]
    assert rows[0]["bid_price"] is None  # empty book: JSON null, not NaN
    assert rows[1]["bid_price"] == 100.0 and rows[1]["ask_price"] is None
    assert rows[2]["ask_price"] == 101.0 and rows[2]["ask_shares"] == 50


def test_bbo_limits(client):
    params = {"start": "00:00:00", "end": "23:00:00", "interval": "100ms"}
    assert client.get("/bbo/AAPL", params=params).status_code == 400
    params["interval"] = "soon"
    assert client.get("/bbo/AAPL", params=params).status_code == 400


def test_stats_endpoint(client):
    stats = client.get("/stats").json()
    assert stats["messages"] == 5
    assert stats["snapshot_minutes"] == 1
    assert stats["server_peak_rss_bytes"] > 0
    assert stats["server_memory_bytes"] > 0
