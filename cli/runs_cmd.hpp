#pragma once

#include "cli/client.hpp"

#include <string>
#include <vector>

namespace zeromesh::cli {

int RunsCommand(Client& client);                                    // zm runs
int AttachCommand(Client& client, const std::string& run_id);       // zm attach ID
int CancelCommand(Client& client, const std::string& run_id);       // zm cancel ID
int ReportCommand(Client& client, const std::vector<std::string>& args); // zm report ID [--json]
int ResumeCommand(Client& client, const std::string& run_id);       // zm resume ID
int CacheCommand(Client& client, const std::vector<std::string>& args);  // zm cache explain|forget ...
int DoctorCommand(Client& client);                                  // zm doctor
int EnvCommand(Client& client, const std::vector<std::string>& args);    // zm env capture ...

// `zm-<name>` executables on PATH: the installed adapters.
std::vector<std::string> InstalledApps();
int AppsCommand();

extern const char* const kRunsUsage;

} // namespace zeromesh::cli
