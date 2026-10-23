// pybind11 bindings: the _itchbook extension module behind `import itchbook`.
//
// Every function copies its result into Python objects or NumPy arrays before
// returning, so nothing in Python points into C++ memory that could be freed.

#include "itchbook/format.hpp"
#include "itchbook/messages.hpp"
#include "itchbook/replay.hpp"
#include "itchbook/snapshot.hpp"

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;

namespace {

constexpr double price_scale = 10'000.0;  // ITCH prices have 4 implied decimals

std::uint64_t parse_time_arg(const std::string& text) {
  const auto t = itchbook::parse_timestamp(text);
  if (!t) {
    throw py::value_error("time must look like HH:MM:SS or HH:MM:SS.nnnnnnnnn, got '" + text + "'");
  }
  return *t;
}

std::uint64_t interval_arg(double seconds) {
  if (!(seconds > 0)) {
    throw py::value_error("interval must be a positive number of seconds");
  }
  return static_cast<std::uint64_t>(std::llround(seconds * 1e9));
}

// Up to depth levels as a list of (price in dollars, shares) tuples.
py::list levels_to_list(const std::vector<itchbook::Level>& levels, std::size_t depth) {
  py::list out;
  for (std::size_t i = 0; i < levels.size() && i < depth; ++i) {
    out.append(py::make_tuple(levels[i].price / price_scale, levels[i].shares));
  }
  return out;
}

// Copies a vector into a new NumPy array.
template <typename Out, typename In>
py::array_t<Out> to_array(const std::vector<In>& values) {
  py::array_t<Out> out(static_cast<py::ssize_t>(values.size()));
  // `template` tells the compiler mutable_unchecked is a template: out's type
  // depends on Out, so it cannot know that yet when it reads this line.
  auto view = out.template mutable_unchecked<1>();
  for (std::size_t i = 0; i < values.size(); ++i) {
    view(static_cast<py::ssize_t>(i)) = static_cast<Out>(values[i]);
  }
  return out;
}

// Prices in dollars, with NaN where the side was empty (raw price 0).
py::array_t<double> to_prices(const std::vector<std::uint32_t>& raw) {
  py::array_t<double> out(static_cast<py::ssize_t>(raw.size()));
  auto view = out.mutable_unchecked<1>();
  for (std::size_t i = 0; i < raw.size(); ++i) {
    view(static_cast<py::ssize_t>(i)) =
        raw[i] == 0 ? std::numeric_limits<double>::quiet_NaN() : raw[i] / price_scale;
  }
  return out;
}

py::dict columns_to_dict(const itchbook::TopOfBookColumns& c) {
  py::dict out;
  out["time"] = to_array<std::int64_t>(c.time);
  out["bid_price"] = to_prices(c.bid_price);
  out["bid_shares"] = to_array<std::int64_t>(c.bid_shares);
  out["ask_price"] = to_prices(c.ask_price);
  out["ask_shares"] = to_array<std::int64_t>(c.ask_shares);
  return out;
}

// One stock's whole book at one time. It holds its own copy of every level.
struct Book {
  std::string symbol;
  std::uint16_t locate;
  std::uint64_t time;
  itchbook::BookTop levels;
};

// BookV1 that also calls a Python function for every message. The GIL stays
// held for the whole replay, since each message calls into Python.
class CallbackBook : public itchbook::BookV1 {
 public:
  explicit CallbackBook(py::function fn) : fn_(std::move(fn)) {}

  void on_message(const itchbook::MessageHeader& h) override {
    fn_(std::string(1, h.type), h.timestamp, h.stock_locate);
  }

 private:
  py::function fn_;
};

}  // namespace

PYBIND11_MODULE(_itchbook, m) {
  m.doc() = "C++ engine for the itchbook package: ITCH 5.0 parsing and order book replay.";

  // A missing or unreadable file is an OSError in Python, not a RuntimeError.
  py::register_exception_translator([](std::exception_ptr p) {
    try {
      if (p) {
        std::rethrow_exception(p);
      }
    } catch (const std::filesystem::filesystem_error& e) {
      PyErr_SetString(PyExc_OSError, e.what());
    }
  });

  py::class_<Book>(m, "Book", "One stock's order book at one time of day.")
      .def_readonly("symbol", &Book::symbol)
      .def_readonly("locate", &Book::locate)
      .def_property_readonly(
          "time", [](const Book& b) { return itchbook::format_timestamp(b.time); },
          "The time of day the book was taken at, as HH:MM:SS.nnnnnnnnn.")
      .def(
          "top",
          [](const Book& b, std::size_t depth) {
            return py::make_tuple(levels_to_list(b.levels.bids, depth),
                                  levels_to_list(b.levels.asks, depth));
          },
          py::arg("depth") = 5,
          "Returns (bids, asks): each a list of up to depth (price, shares) tuples, best first.")
      .def("__repr__", [](const Book& b) {
        return "<Book " + b.symbol + " at " + itchbook::format_timestamp(b.time) + ": " +
               std::to_string(b.levels.bids.size()) + " bid levels, " +
               std::to_string(b.levels.asks.size()) + " ask levels>";
      });

  m.def(
      "replay",
      [](const std::filesystem::path& path, const std::string& symbol, const std::string& until) {
        const std::uint64_t time = parse_time_arg(until);
        // Keep every level, so Book.top can take any depth later.
        itchbook::BookAt book(symbol, time, std::numeric_limits<std::size_t>::max());
        {
          // The replay touches no Python objects, so other Python threads can run.
          py::gil_scoped_release release;
          itchbook::parse_file(path, book);
        }
        const auto locate = book.locate();
        if (!locate) {
          throw py::value_error("no R message names " + symbol);
        }
        return Book{symbol, *locate, time, book.result()};
      },
      py::arg("path"), py::arg("symbol"), py::arg("until"),
      "Replays path up to until (HH:MM:SS) and returns symbol's Book as it stood then.");

  m.def(
      "top_of_book_series",
      [](const std::filesystem::path& path, const std::string& symbol, double interval) {
        itchbook::TopOfBookSeries series(symbol, interval_arg(interval));
        {
          py::gil_scoped_release release;
          itchbook::parse_file(path, series);
        }
        if (!series.locate()) {
          throw py::value_error("no R message names " + symbol);
        }
        return columns_to_dict(series.columns());
      },
      py::arg("path"), py::arg("symbol"), py::arg("interval"),
      "Replays the whole file and samples symbol's best bid and ask every interval seconds.\n"
      "Returns a dict of NumPy arrays: time (ns since midnight), bid_price, bid_shares,\n"
      "ask_price, ask_shares. A sample at time T includes every message stamped T or earlier.");

  py::class_<itchbook::SnapshotIndex>(
      m, "SnapshotIndex",
      "Answers book queries without replaying from the start of the day.\n"
      "Built by one replay that saves each symbol's book every interval seconds of\n"
      "exchange time; interval 0 saves none, so every query replays from the start.")
      .def(py::init([](const std::filesystem::path& path, const std::vector<std::string>& symbols,
                       double interval) {
             if (interval < 0) {
               throw py::value_error("interval must be 0 or a positive number of seconds");
             }
             const std::uint64_t ns = interval == 0 ? 0 : interval_arg(interval);
             py::gil_scoped_release release;
             return std::make_unique<itchbook::SnapshotIndex>(path, symbols, ns);
           }),
           py::arg("path"), py::arg("symbols"), py::arg("interval"))
      .def("symbols", &itchbook::SnapshotIndex::symbols,
           "The requested symbols that the file names, in the order given.")
      .def(
          "book",
          [](const itchbook::SnapshotIndex& index, const std::string& symbol,
             const std::string& at, std::size_t depth) {
            const std::uint64_t time = parse_time_arg(at);
            std::optional<itchbook::BookTop> top;
            {
              py::gil_scoped_release release;
              top = index.book(symbol, time, depth);
            }
            if (!top) {
              throw py::key_error(symbol + " is not indexed");
            }
            return py::make_tuple(levels_to_list(top->bids, depth),
                                  levels_to_list(top->asks, depth));
          },
          py::arg("symbol"), py::arg("at"), py::arg("depth") = 5,
          "Returns (bids, asks) at time at, like Book.top. KeyError if symbol is not indexed.")
      .def(
          "bbo",
          [](const itchbook::SnapshotIndex& index, const std::string& symbol,
             const std::string& start, const std::string& end, double interval) {
            const std::uint64_t from = parse_time_arg(start);
            const std::uint64_t to = parse_time_arg(end);
            const std::uint64_t step = interval_arg(interval);
            std::optional<itchbook::TopOfBookColumns> columns;
            {
              py::gil_scoped_release release;
              columns = index.bbo(symbol, from, to, step);
            }
            if (!columns) {
              throw py::key_error(symbol + " is not indexed");
            }
            py::dict out = columns_to_dict(*columns);
            py::list clock;
            for (const std::uint64_t t : columns->time) {
              clock.append(itchbook::format_timestamp(t));
            }
            out["clock"] = clock;
            return out;
          },
          py::arg("symbol"), py::arg("start"), py::arg("end"), py::arg("interval"),
          "Best bid and ask every interval seconds from start to end, like\n"
          "top_of_book_series, plus clock: each time as HH:MM:SS.nnnnnnnnn.")
      .def("stats", [](const itchbook::SnapshotIndex& index) {
        const auto& s = index.stats();
        py::dict by_type;
        for (std::size_t type = 0; type < s.by_type.size(); ++type) {
          if (s.by_type[type] != 0) {
            by_type[py::str(std::string(1, static_cast<char>(type)))] = s.by_type[type];
          }
        }
        py::dict out;
        out["messages"] = s.messages;
        out["by_type"] = by_type;
        out["symbols"] = index.symbols();
        out["interval_seconds"] = s.interval / 1e9;
        out["snapshots"] = s.snapshots;
        out["snapshot_bytes"] = s.snapshot_bytes;
        out["build_seconds"] = s.build_seconds;
        return out;
      });

  m.def(
      "replay_with_callback",
      [](const std::filesystem::path& path, py::function fn) {
        CallbackBook book(std::move(fn));
        itchbook::parse_file(path, book);
      },
      py::arg("path"), py::arg("fn"),
      "Replays the whole file, calling fn(type, timestamp, locate) once per message.\n"
      "An experiment in the cost of crossing into Python per message; it is slow by design.");
}
