// Copyright 2016, University of Freiburg,
// Chair of Algorithms and Data Structures.
// Authors: Patrick Brosi <brosi@informatik.uni-freiburg.de>

#include <float.h>
#include <getopt.h>
#include <exception>
#include <iostream>
#include <string>
#include "ad/cppgtfs/gtfs/flat/Route.h"
#include "gtfs2graph/_config.h"
#include "gtfs2graph/config/ConfigReader.h"
#include "util/String.h"
#include "util/log/Log.h"

using gtfs2graph::config::ConfigReader;

using std::exception;
using std::string;
using std::vector;

static const char* YEAR = &__DATE__[7];
static const char* COPY =
    "University of Freiburg - Chair of Algorithms and Data Structures";
static const char* AUTHORS = "Patrick Brosi <brosi@informatik.uni-freiburg.de>";

// _____________________________________________________________________________
ConfigReader::ConfigReader() {}

// _____________________________________________________________________________
void ConfigReader::help(const char* bin) const {
  std::cout << std::setfill(' ') << std::left << "gtfs2graph (part of LOOM) "
            << VERSION_FULL << "\n(built " << __DATE__ << " " << __TIME__ << ")"
            << "\n\n(C) " << YEAR << " " << COPY << "\n"
            << "Authors: " << AUTHORS << "\n\n"
            << "Usage: " << bin << " <GTFS FEED>\n\n"
            << "Allowed options:\n\n"
            << "General:\n"
            << std::setw(35) << "  -v [ --version ]"
            << "print version\n"
            << std::setw(35) << "  -h [ --help ]"
            << "show this help message\n"
            << std::setw(35) << "  -m [ --mots ] arg (=all)"
            << "MOTs to calculate shapes for, comma sep.,\n"
            << std::setw(35) << " "
            << "  either as string "
               "{all, tram | streetcar,\n"
            << std::setw(35) << " "
            << "  subway | metro, rail | train, bus,\n"
            << std::setw(35) << " "
            << "  ferry | boat | ship, cablecar, gondola,\n"
            << std::setw(35) << " "
            << "  funicular, coach} or as GTFS mot codes\n";
}

// _____________________________________________________________________________
void ConfigReader::read(Config* cfg, int argc, char** argv) const {
  std::string motStr = "all";

  struct option ops[] = {{"version", no_argument, 0, 'v'},
                         {"help", no_argument, 0, 'h'},
                         {"mots", required_argument, 0, 'm'},
                         {0, 0, 0, 0}};

  char c;
  while ((c = getopt_long(argc, argv, ":hvim:", ops, 0)) != -1) {
    switch (c) {
      case 'h':
        help(argv[0]);
        exit(0);
      case 'v':
        std::cout << "gtfs2graph - (LOOM " << VERSION_FULL << ")" << std::endl;
        exit(0);
      case 'm':
        motStr = optarg;
        break;
      case ':':
        std::cerr << argv[optind - 1];
        std::cerr << " requires an argument" << std::endl;
        exit(1);
      case '?':
        std::cerr << argv[optind - 1];
        std::cerr << " option unknown" << std::endl;
        exit(1);
        break;
      default:
        std::cerr << "Error while parsing arguments" << std::endl;
        exit(1);
        break;
    }
  }

  if (optind == argc) {
      std::cerr << "No input GTFS feed specified." << std::endl;
      exit(1);
  }

  cfg->inputFeedPath = argv[optind];

  for (auto sMotStr : util::split(motStr, ',')) {
    for (auto mot :
         ad::cppgtfs::gtfs::flat::Route::getTypesFromString(sMotStr)) {
      cfg->useMots.insert(mot);
    }
  }
}
