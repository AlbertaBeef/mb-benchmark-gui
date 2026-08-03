#include <gtkmm/application.h>

#include "MainWindow.h"

int main(int argc, char* argv[]) {
    auto app = Gtk::Application::create("org.albertabeef.mbbenchmark");
    return app->make_window_and_run<MainWindow>(argc, argv);
}
