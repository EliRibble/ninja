// Copyright 2019 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "public/ui.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "getopt.h"
#include <direct.h>
#include <windows.h>
#elif defined(_AIX)
#include "getopt.h"
#include <unistd.h>
#else
#include <getopt.h>
#include <unistd.h>
#endif

#include <iostream>
#include <vector>

#include "public/execution.h"
#include "public/logger.h"
#include "public/version.h"

#include "build.h"
#include "build_log.h"
#include "deps_log.h"
#include "debug_flags.h"
#include "disk_interface.h"
#include "graph.h"
#include "manifest_parser.h"
#include "metrics.h"
#include "status.h"
#include "util.h"


namespace ninja {
namespace ui {
namespace {

const char kLogError[] = "ninja: error: ";
const char kLogInfo[] = "ninja: ";
const char kLogWarning[] = "ninja: warning: ";

/// Enable a debugging mode.  Returns false if Ninja should exit instead
/// of continuing.
bool DebugEnable(const string& name) {
  if (name == "list") {
    printf("debugging modes:\n"
"  stats        print operation counts/timing info\n"
"  explain      explain what caused a command to execute\n"
"  keepdepfile  don't delete depfiles after they're read by ninja\n"
"  keeprsp      don't delete @response files on success\n"
#ifdef _WIN32
"  nostatcache  don't batch stat() calls per directory and cache them\n"
#endif
"multiple modes can be enabled via -d FOO -d BAR\n");
    return false;
  } else if (name == "stats") {
    g_metrics = new Metrics;
    return true;
  } else if (name == "explain") {
    g_explaining = true;
    return true;
  } else if (name == "keepdepfile") {
    g_keep_depfile = true;
    return true;
  } else if (name == "keeprsp") {
    g_keep_rsp = true;
    return true;
  } else if (name == "nostatcache") {
    g_experimental_statcache = false;
    return true;
  } else {
    const char* suggestion =
        SpellcheckString(name.c_str(),
                         "stats", "explain", "keepdepfile", "keeprsp",
                         "nostatcache", NULL);
    std::cerr << kLogError << "unknown debug setting '" << name << "'";
    if (suggestion) {
      std::cerr << ", did you mean '" << suggestion << "'?";
    }
    std::cerr << endl;
    return false;
  }
}

/// Set a warning flag.  Returns false if Ninja should exit instead  of
/// continuing.
bool WarningEnable(const string& name, Execution::Options* options) {
  if (name == "list") {
    printf("warning flags:\n"
"  dupbuild={err,warn}  multiple build lines for one target\n"
"  phonycycle={err,warn}  phony build statement references itself\n"
"  depfilemulti={err,warn}  depfile has multiple output paths on separate lines\n"
    );
    return false;
  } else if (name == "dupbuild=err") {
    options->dupe_edges_should_err = true;
    return true;
  } else if (name == "dupbuild=warn") {
    options->dupe_edges_should_err = false;
    return true;
  } else if (name == "phonycycle=err") {
    options->phony_cycle_should_err = true;
    return true;
  } else if (name == "phonycycle=warn") {
    options->phony_cycle_should_err = false;
    return true;
  } else if (name == "depfilemulti=err") {
    options->depfile_distinct_target_lines_should_err = true;
    return true;
  } else if (name == "depfilemulti=warn") {
    options->depfile_distinct_target_lines_should_err = false;
    return true;
  } else {
    const char* suggestion =
        SpellcheckString(name.c_str(), "dupbuild=err", "dupbuild=warn",
                         "phonycycle=err", "phonycycle=warn", NULL);
    std::cerr << ui::kLogError << "unknown warning flag '" << name << "'";
    if (suggestion) {
      std::cerr << ", did you mean '" << suggestion << "'?";
    }
    std::cerr << std::endl;
    return false;
  }
}

}  // namespace

const char* Error() { return kLogError; }
const char* Info() { return kLogInfo; }
const char* Warning() { return kLogWarning; }

NORETURN void Execute(int argc, char** argv) {
  setvbuf(stdout, NULL, _IOLBF, BUFSIZ);

  ninja::Execution::Options options;
  int exit_code = ui::ReadFlags(&argc, &argv, &options);
  if (exit_code >= 0)
    exit(exit_code);

  const char* ninja_command = argv[0];
  ninja::Execution execution(ninja_command, options);


  if (execution.options().working_dir) {
    // The formatting of this string, complete with funny quotes, is
    // so Emacs can properly identify that the cwd has changed for
    // subsequent commands.
    // Don't print this if a tool is being used, so that tool output
    // can be piped into a file without this string showing up.
    if (!execution.options().tool_)
      std::cerr << ui::Info() << "Entering directory `" << execution.options().working_dir << "'" << std::endl;
    if (chdir(execution.options().working_dir) < 0) {
      std::cerr << ui::Error() << "chdir to '" << execution.options().working_dir << "' - " << strerror(errno) << std::endl;
      exit(1);
    }
  }

  if (execution.options().tool_ && execution.options().tool_->when == Tool::RUN_AFTER_FLAGS) {
    // None of the RUN_AFTER_FLAGS actually use a ninja state, but it's needed
    // by other tools.
    exit((execution.options().tool_->func)(&execution, argc, argv));
  }

  Status* status = new StatusPrinter(execution.config());

  // Limit number of rebuilds, to prevent infinite loops.
  const int kCycleLimit = 100;
  for (int cycle = 1; cycle <= kCycleLimit; ++cycle) {

    ManifestParserOptions parser_opts;
    if (execution.options().dupe_edges_should_err) {
      parser_opts.dupe_edge_action_ = kDupeEdgeActionError;
    }
    if (execution.options().phony_cycle_should_err) {
      parser_opts.phony_cycle_action_ = kPhonyCycleActionError;
    }
    ManifestParser parser(execution.state_, execution.DiskInterface(), parser_opts);
    string err;
    if (!parser.Load(execution.options().input_file, &err)) {
      status->Error("%s", err.c_str());
      exit(1);
    }

    if (execution.options().tool_ && execution.options().tool_->when == Tool::RUN_AFTER_LOAD)
      exit((execution.options().tool_->func)(&execution, argc, argv));

    if (!EnsureBuildDirExists(&execution, execution.DiskInterface(), execution.config(), &err))
      exit(1);

    if (!OpenBuildLog(&execution, execution.config(), false, &err) || !OpenDepsLog(&execution, execution.config(), false, &err)) {
      std::cerr << ui::Error() << err << std::endl;
      exit(1);
    }

    // Hack: OpenBuildLog()/OpenDepsLog() can return a warning via err
    if(!err.empty()) {
      std::cerr << ui::Warning() << err << std::endl;
      err.clear();
    }

    if (execution.options().tool_ && execution.options().tool_->when == Tool::RUN_AFTER_LOGS)
      exit((execution.options().tool_->func)(&execution, argc, argv));

    // Attempt to rebuild the manifest before building anything else
    if (RebuildManifest(&execution, execution.options().input_file, &err, status)) {
      // In dry_run mode the regeneration will succeed without changing the
      // manifest forever. Better to return immediately.
      if (execution.config().dry_run)
        exit(0);
      // Start the build over with the new manifest.
      continue;
    } else if (!err.empty()) {
      status->Error("rebuilding '%s': %s", execution.options().input_file, err.c_str());
      exit(1);
    }

    int result = RunBuild(&execution, argc, argv, status);
    if (g_metrics)
      execution.DumpMetrics();
    exit(result);
  }

  status->Error("manifest '%s' still dirty after %d tries",
      execution.options().input_file, kCycleLimit);
  exit(1);
}
void ExitNow() {
#ifdef _WIN32
  // On Windows, some tools may inject extra threads.
  // exit() may block on locks held by those threads, so forcibly exit.
  fflush(stderr);
  fflush(stdout);
  ExitProcess(1);
#else
  exit(1);
#endif
}

/// Parse argv for command-line options.
/// Returns an exit code, or -1 if Ninja should continue.
int ReadFlags(int* argc, char*** argv,
              Execution::Options* options) {

  enum { OPT_VERSION = 1 };
  const option kLongOptions[] = {
    { "help", no_argument, NULL, 'h' },
    { "version", no_argument, NULL, OPT_VERSION },
    { "verbose", no_argument, NULL, 'v' },
    { NULL, 0, NULL, 0 }
  };

  int opt;
  while (!options->tool_ &&
         (opt = getopt_long(*argc, *argv, "d:f:j:k:l:nt:vw:C:h", kLongOptions,
                            NULL)) != -1) {
    switch (opt) {
      case 'd':
        if (!DebugEnable(optarg))
          return 1;
        break;
      case 'f':
        options->input_file = optarg;
        break;
      case 'j': {
        char* end;
        int value = strtol(optarg, &end, 10);
        if (*end != 0 || value < 0) {
          std::cerr << "invalid -j parameter" << std::endl;
          ui::ExitNow();
        }

        // We want to run N jobs in parallel. For N = 0, INT_MAX
        // is close enough to infinite for most sane builds.
        options->parallelism = value > 0 ? value : INT_MAX;
        break;
      }
      case 'k': {
        char* end;
        int value = strtol(optarg, &end, 10);
        if (*end != 0) {
          std::cerr << "-k parameter not numeric; did you mean -k 0?" << std::endl;
          ui::ExitNow();
        }

        // We want to go until N jobs fail, which means we should allow
        // N failures and then stop.  For N <= 0, INT_MAX is close enough
        // to infinite for most sane builds.
        options->failures_allowed = value > 0 ? value : INT_MAX;
        break;
      }
      case 'l': {
        char* end;
        double value = strtod(optarg, &end);
        if (end == optarg) {
          std::cerr << "-l parameter not numeric: did you mean -l 0.0?" << std::endl;
          ui::ExitNow();
        }
        options->max_load_average = value;
        break;
      }
      case 'n':
        options->dry_run = true;
        break;
      case 't':
        options->tool_ = tool::Choose(optarg);
        if(options->tool_ == NULL) {
          const char* suggestion = GetToolNameSuggestion(optarg);
          std::cerr << "unknown tool '" << optarg << "'";
          if (suggestion) {
            std::cerr << ", did you mean '" << suggestion << "'?";
          }
          std::cerr << std::endl;
          return 1;
        }
        break;
      case 'v':
        options->verbose = true;
        break;
      case 'w':
        if (!WarningEnable(optarg, options))
          return 1;
        break;
      case 'C':
        options->working_dir = optarg;
        break;
      case OPT_VERSION:
        printf("%s\n", kNinjaVersion);
        return 0;
      case 'h':
      default:
        ui::Usage(options);
        return 1;
    }
  }
  *argv += optind;
  *argc -= optind;

  return -1;
}

// Get a suggested tool name given a name that is supposed
// to be like a tool.
const char* GetToolNameSuggestion(const std::string& tool_name) {
  std::vector<const char*> words = tool::AllNames();
  return SpellcheckStringV(tool_name, words);
}

void Usage(const Execution::Options* options) {
  fprintf(stderr,
"usage: ninja [options] [targets...]\n"
"\n"
"if targets are unspecified, builds the 'default' target (see manual).\n"
"\n"
"options:\n"
"  --version      print ninja version (\"%s\")\n"
"  -v, --verbose  show all command lines while building\n"
"\n"
"  -C DIR   change to DIR before doing anything else\n"
"  -f FILE  specify input build file [default=build.ninja]\n"
"\n"
"  -j N     run N jobs in parallel (0 means infinity) [default=%d on this system]\n"
"  -k N     keep going until N jobs fail (0 means infinity) [default=1]\n"
"  -l N     do not start new jobs if the load average is greater than N\n"
"  -n       dry run (don't run commands but act like they succeeded)\n"
"\n"
"  -d MODE  enable debugging (use '-d list' to list modes)\n"
"  -t TOOL  run a subtool (use '-t list' to list subtools)\n"
"    terminates toplevel options; further flags are passed to the tool\n"
"  -w FLAG  adjust warnings (use '-w list' to list warnings)\n",
          kNinjaVersion, options->parallelism);
}

}  // namespace ui
}  // namespace ninja
