#include <gtkmm/application.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Automation.h"
#include "MainWindow.h"

namespace {

void usage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s [--automation <file>]\n"
        "\n"
        "  --automation <file>  run an unattended schedule read from <file>.\n"
        "                       See config/automation-models.conf for the\n"
        "                       format and a ready-made model sweep.\n",
        argv0);
}

}  // namespace

int main(int argc, char* argv[]) {
    AutomationPlan plan;

    // Strip our own options before GTK sees argv: Gtk::Application treats an
    // unknown long option as an error and exits, so --automation has to be
    // removed here rather than parsed alongside.
    std::vector<char*> passthrough;
    passthrough.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        std::string value;
        bool matched = false;
        if (a.rfind("--automation=", 0) == 0) {
            value = a.substr(std::strlen("--automation="));
            matched = true;
        } else if (a == "--automation") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --automation needs a file\n");
                usage(argv[0]);
                return 2;
            }
            value = argv[++i];
            matched = true;
        } else if (a == "--help" || a == "-h") {
            usage(argv[0]);
            return 0;
        }
        if (!matched) {
            passthrough.push_back(argv[i]);
            continue;
        }
        std::string err;
        if (!load_automation_plan(value, plan, err)) {
            // Fatal rather than a warning: the user asked for an unattended
            // session and would otherwise come back hours later to an idle app.
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        std::fprintf(stderr, "automation: %s — %zu step(s), %s\n",
                     plan.path.c_str(), plan.steps.size(),
                     plan.loop ? "looping" : "one pass");
    }

    int pargc = static_cast<int>(passthrough.size());
    auto app = Gtk::Application::create("org.albertabeef.mbbenchmark");
    return app->make_window_and_run<MainWindow>(pargc, passthrough.data(),
                                                std::move(plan));
}
