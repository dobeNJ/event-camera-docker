/*******************************************************************
 * File : prophesee_viewer.cpp  (patched for signal-based recording)
 *
 * Changes vs original:
 *   - Added SIGUSR1 handler → starts recording  (like pressing SPACE)
 *   - Added SIGUSR2 handler → stops  recording  (like pressing SPACE)
 *   - Writes PID file to /tmp/prophesee_viewer.pid on startup
 *   - Writes recording status to /tmp/prophesee_status.txt
 *     ("READY", "RECORDING", "STOPPED")
 *
 * Copyright: (c) 2015-2019 Prophesee
 *******************************************************************/

#include <iostream>
#include <fstream>
#include <csignal>
#include <atomic>
#include <boost/program_options.hpp>
#include <prophesee_driver.h>
#include <utils/tone_mapper.h>
#include <thread>
#include <chrono>
#if CV_MAJOR_VERSION >= 4
#include <opencv2/highgui/highgui_c.h>
#endif

#include "utils/cd_frame_generator.h"
#include "utils/em_frame_generator.h"

static const int ESCAPE = 27;
static const int SPACE  = 32;

// ---------------------------------------------------------------------------
// Signal-based recording control
// ---------------------------------------------------------------------------
static const char *PID_FILE    = "/tmp/prophesee_viewer.pid";
static const char *STATUS_FILE = "/tmp/prophesee_status.txt";

// Flags set by signal handlers, consumed in the main loop
static std::atomic<bool> g_sig_start_recording{false};
static std::atomic<bool> g_sig_stop_recording{false};

static void handle_sigusr1(int) { g_sig_start_recording.store(true);  }
static void handle_sigusr2(int) { g_sig_stop_recording.store(true);   }

static void write_status(const std::string &status) {
    std::ofstream f(STATUS_FILE, std::ios::trunc);
    if (f) f << status << std::endl;
}

static void write_pid() {
    std::ofstream f(PID_FILE, std::ios::trunc);
    if (f) f << getpid() << std::endl;
}

static void cleanup_files() {
    std::remove(PID_FILE);
    std::remove(STATUS_FILE);
}

// ---------------------------------------------------------------------------

namespace po = boost::program_options;

int process_ui_for(int delay_ms) {
    auto then = std::chrono::high_resolution_clock::now();
    int key   = cv::waitKey(delay_ms);
    auto now  = std::chrono::high_resolution_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(
        delay_ms - std::chrono::duration_cast<std::chrono::milliseconds>(now - then).count()));
    return key;
}

bool window_was_closed(const std::string &window_name) {
#if CV_MAJOR_VERSION >= 3 && CV_MINOR_VERSION >= 2
    if (cv::getWindowProperty(window_name, cv::WND_PROP_VISIBLE) == 0) {
#else
    if (cv::getWindowProperty(window_name, cv::WND_PROP_AUTOSIZE) != 0) {
#endif
        return true;
    }
    return false;
}

int setup_cd_callback_and_window(Prophesee::Camera &camera, cv::Mat &cd_frame,
                                 Prophesee::CDFrameGenerator &cd_frame_generator,
                                 const std::string &window_name) {
    auto &geometry = camera.geometry();
    auto id        = camera.cd().add_callback(
        [&cd_frame_generator](const Prophesee::EventCD *ev_begin, const Prophesee::EventCD *ev_end) {
            cd_frame_generator.add_events(ev_begin, ev_end);
        });
    cd_frame_generator.start(
        30, [&cd_frame](const Prophesee::timestamp &ts, const cv::Mat &frame) { frame.copyTo(cd_frame); });
    cv::namedWindow(window_name, CV_GUI_EXPANDED);
    cv::resizeWindow(window_name, geometry.width(), geometry.height());
    cv::moveWindow(window_name, 0, 0);
    return id;
}

int setup_em_callback_and_window(Prophesee::Camera &camera, cv::Mat &em_frame,
                                 Prophesee::EMFrameGenerator &em_frame_generator,
                                 const std::string &window_name) {
    auto &geometry = camera.geometry();
    try {
        auto id = camera.em().add_callback(
            [&em_frame_generator](const Prophesee::EventEM *ev_begin, const Prophesee::EventEM *ev_end) {
                em_frame_generator.add_events(ev_begin, ev_end);
            });
        em_frame_generator.start(
            30, [&em_frame](const Prophesee::timestamp &ts, const cv::Mat &frame) { frame.copyTo(em_frame); });
        cv::namedWindow(window_name, CV_GUI_EXPANDED);
        cv::resizeWindow(window_name, geometry.width(), geometry.height());
        cv::moveWindow(window_name, geometry.width() + 100, geometry.height() + 100);
        return id;
    } catch (Prophesee::CameraException &e) {
        if (e.code().value() & Prophesee::CameraErrorCode::UnsupportedFeature)
            return -1;
        else
            std::cerr << e.what() << std::endl;
    }
    return -2;
}

int setup_graylevel_frame_callback_and_window(Prophesee::Camera &camera, cv::Mat &graylevel_frame,
                                              Prophesee::Utils::GammaToneMapper &tone_mapper,
                                              const std::string &window_name) {
    auto &geometry = camera.geometry();
    try {
        camera.set_exposure_frame_callback(
            30, [&graylevel_frame, &tone_mapper](Prophesee::timestamp ts, const cv::Mat &f) {
                tone_mapper(f, graylevel_frame);
            });
        cv::namedWindow(window_name, CV_GUI_EXPANDED);
        cv::resizeWindow(window_name, geometry.width(), geometry.height());
        cv::moveWindow(window_name, geometry.width() + 100, 0);
        return 0;
    } catch (Prophesee::CameraException &e) {
        if (e.code().value() & Prophesee::CameraErrorCode::UnsupportedFeature)
            return -1;
        else
            std::cerr << e.what() << std::endl;
    }
    return -2;
}

bool is_imu_available(Prophesee::Camera &camera) {
    try { camera.imu_sensor(); } catch (Prophesee::CameraException &) { return false; }
    return true;
}

int main(int argc, char *argv[]) {
    // --- Install signal handlers -------------------------------------------
    std::signal(SIGUSR1, handle_sigusr1);
    std::signal(SIGUSR2, handle_sigusr2);

    // Write PID so the orchestrator can find us
    write_pid();
    write_status("READY");

    // Cleanup on normal exit
    std::atexit(cleanup_files);

    // -----------------------------------------------------------------------

    std::string serial;
    std::string biases_file;
    std::string filename;
    std::string filename_to_log_into;
    std::vector<uint16_t> roi;
    uint32_t max_rate_kEV_s = 10000;

    bool do_retry = false;

    const std::string program_desc =
        "\nSimple viewer to stream events from a prophesee rawfile or device.\n\n"
        "Press SPACE key while running to record or stop recording raw data\n"
        "Send SIGUSR1 to start recording programmatically.\n"
        "Send SIGUSR2 to stop  recording programmatically.\n"
        "PID is written to /tmp/prophesee_viewer.pid\n"
        "Status is written to /tmp/prophesee_status.txt  (READY | RECORDING | STOPPED)\n"
        "Press 'q' or Escape key to leave the program.\n";

    po::options_description desc(program_desc + "\nAllowed options");
    desc.add_options()
        ("help,h",    "Print this help message")
        ("serial,s",  po::value<std::string>(&serial)->default_value(""),              "Serial ID of the camera.")
        ("filename,f",po::value<std::string>(&filename)->default_value(""),            "Path to a rawfile to read.")
        ("biases,b",  po::value<std::string>(&biases_file)->default_value(""),         "Path to a biases file.")
        ("max-rate",  po::value<uint32_t>(&max_rate_kEV_s)->default_value(10000),      "Max event rate [kev/s].")
        ("output,o",  po::value<std::string>(&filename_to_log_into)->default_value("data.raw"),
                      "Path to output rawfile for recording.")
        ("roi,r",     po::value<std::vector<uint16_t>>(&roi)->multitoken(),            "Hardware ROI: x y width height");

    po::variables_map vm;
    try {
        po::store(po::command_line_parser(argc, argv).options(desc).run(), vm);
        po::notify(vm);
    } catch (...) {
        std::cout << desc << std::endl;
        return 1;
    }

    if (vm.count("help")) { std::cout << desc << std::endl; return 0; }

    if (vm.count("roi") && roi.size() != 4) {
        std::cerr << "Warning: ROI must be 'x y width height'. Ignored." << std::endl;
        roi.clear();
    }

    std::cout << program_desc << std::endl;
    std::cout << "PID: " << getpid() << "  →  " << PID_FILE << std::endl;

    do {
        Prophesee::Camera camera;
        bool camera_is_opened = false;

        if (filename != "") {
            if (!serial.empty()) {
                std::cerr << "Error: --serial and --filename are incompatible." << std::endl;
                return 1;
            }
            try {
                camera           = Prophesee::Camera::from_file(filename);
                camera_is_opened = true;
            } catch (Prophesee::CameraException &e) { std::cerr << e.what() << std::endl; }
        } else {
            try {
                camera = serial.empty() ? Prophesee::Camera::from_first_available()
                                        : Prophesee::Camera::from_serial(serial);
                if (biases_file != "")  camera.biases().set_from_file(biases_file);
                if (!roi.empty())       camera.roi().set({roi[0], roi[1], roi[2], roi[3]});
                camera_is_opened = true;
            } catch (Prophesee::CameraException &e) { std::cerr << e.what() << std::endl; }
        }

        if (!camera_is_opened) {
            if (do_retry) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                std::cout << "Trying to reopen camera..." << std::endl;
                continue;
            } else {
                return -1;
            }
        }
        std::cout << "Camera opened successfully." << std::endl;

        camera.add_runtime_error_callback([&do_retry](const Prophesee::CameraException &e) {
            std::cerr << e.what() << std::endl;
            do_retry = true;
        });

        auto &geometry = camera.geometry();

        const bool cd_available        = true;
        bool cd_frame_activated        = cd_available;
        std::string cd_window_name("CD Events");
        cv::Mat cd_frame;
        Prophesee::CDFrameGenerator cd_frame_generator(geometry.width(), geometry.height());
        cd_frame_generator.set_display_accumulation_time_us(10000);
        int cd_events_cb_id = setup_cd_callback_and_window(camera, cd_frame, cd_frame_generator, cd_window_name);

        const bool em_available            = camera.generation().type() == Prophesee::CameraGeneration::Type::EM;
        bool graylevel_frame_activated     = em_available;
        std::string graylevel_window_name("GrayLevel frame");
        cv::Mat graylevel_frame;
        Prophesee::Utils::GammaToneMapper tone_mapper;
        int graylevel_frame_cb_ret = em_available ?
            setup_graylevel_frame_callback_and_window(camera, graylevel_frame, tone_mapper, graylevel_window_name) : -1;

        bool em_frame_activated = false;
        std::string em_window_name("EM Events");
        cv::Mat em_frame;
        Prophesee::EMFrameGenerator em_frame_generator(geometry.width(), geometry.height());
        int em_events_cb_id = -1;

        const bool imu_available = is_imu_available(camera);
        bool show_imu_data(false);
        Prophesee::CallbackId imu_id(std::numeric_limits<Prophesee::CallbackId>::max());

        camera.start();

        bool recording             = false;
        bool is_roi_set            = true;
        bool max_event_rate_active = false;

        while (camera.is_running()) {
            // --- Handle programmatic recording signals ---------------------
            if (g_sig_start_recording.exchange(false)) {
                if (!recording) {
                    camera.start_recording(filename_to_log_into);
                    recording = true;
                    write_status("RECORDING");
                    std::cout << "\n[SIGNAL] Recording STARTED → " << filename_to_log_into << std::endl;
                }
            }
            if (g_sig_stop_recording.exchange(false)) {
                if (recording) {
                    camera.stop_recording();
                    recording = false;
                    write_status("STOPPED");
                    std::cout << "\n[SIGNAL] Recording STOPPED." << std::endl;
                }
            }

            // --- Render frames --------------------------------------------
            if (!cd_frame_activated && !em_frame_activated && !graylevel_frame_activated) {
                cd_frame_activated = true;
                cd_events_cb_id    = setup_cd_callback_and_window(camera, cd_frame, cd_frame_generator, cd_window_name);
            }
            if (cd_frame_activated         && !cd_frame.empty())         cv::imshow(cd_window_name,        cd_frame);
            if (em_frame_activated         && !em_frame.empty())         cv::imshow(em_window_name,        em_frame);
            if (graylevel_frame_activated  && !graylevel_frame.empty())  cv::imshow(graylevel_window_name, graylevel_frame);

            // --- Keyboard -------------------------------------------------
            int key = process_ui_for(33);
            switch (key) {
            case 'q':
            case ESCAPE:
                camera.stop();
                do_retry = false;
                break;
            case SPACE:
                if (!recording) {
                    camera.start_recording(filename_to_log_into);
                    write_status("RECORDING");
                    std::cout << "[KEY] Recording STARTED → " << filename_to_log_into << std::endl;
                } else {
                    camera.stop_recording();
                    write_status("STOPPED");
                    std::cout << "[KEY] Recording STOPPED." << std::endl;
                }
                recording = !recording;
                break;
            case 'c':
                if (cd_frame_activated) {
                    camera.cd().remove_callback(static_cast<Prophesee::CallbackId>(cd_events_cb_id));
                    cd_frame_generator.stop();
                    cv::destroyWindow(cd_window_name);
                } else {
                    cd_events_cb_id = setup_cd_callback_and_window(camera, cd_frame, cd_frame_generator, cd_window_name);
                }
                cd_frame_activated = !cd_frame_activated;
                break;
            case 'r':
                if (!roi.empty()) {
                    if (!is_roi_set) camera.roi().set({roi[0], roi[1], roi[2], roi[3]});
                    else             camera.roi().unset();
                    is_roi_set = !is_roi_set;
                }
                break;
            case 'i':
                if (imu_available) {
                    if (show_imu_data) {
                        camera.imu().remove_callback(imu_id);
                        if (filename.empty()) camera.imu_sensor().disable();
                    } else {
                        if (filename.empty()) camera.imu_sensor().enable();
                        imu_id = camera.imu().add_callback(
                            [](const Prophesee::EventIMU *begin, const Prophesee::EventIMU *end) {
                                for (auto it = begin; it != end; ++it)
                                    std::cout << "(ax,ay,az,gx,gy,gz): ("
                                              << it->ax << "," << it->ay << "," << it->az << ","
                                              << it->gx << "," << it->gy << "," << it->gz << ")\n";
                            });
                    }
                    show_imu_data = !show_imu_data;
                }
                break;
            case 'e':
                if (em_available) {
                    if (em_frame_activated) {
                        camera.em().remove_callback(static_cast<Prophesee::CallbackId>(em_events_cb_id));
                        em_frame_generator.stop();
                        cv::destroyWindow(em_window_name);
                        em_frame_activated = false;
                    } else {
                        em_events_cb_id    = setup_em_callback_and_window(camera, em_frame, em_frame_generator, em_window_name);
                        em_frame_activated = (em_events_cb_id >= 0);
                    }
                }
                break;
            case 'f':
                if (em_available) {
                    if (graylevel_frame_activated) {
                        camera.unset_exposure_frame_callback();
                        cv::destroyWindow(graylevel_window_name);
                        graylevel_frame_activated = false;
                    } else {
                        graylevel_frame_cb_ret = setup_graylevel_frame_callback_and_window(
                            camera, graylevel_frame, tone_mapper, graylevel_window_name);
                        graylevel_frame_activated = (graylevel_frame_cb_ret >= 0);
                    }
                }
                break;
            case 'd':
                camera.set_max_event_rate_limit(max_event_rate_active ? 0 : max_rate_kEV_s);
                max_event_rate_active = !max_event_rate_active;
                break;
            case 'h':
                std::cout << program_desc << std::endl;
                break;
            default:
                break;
            }
        }

        if (cd_events_cb_id >= 0)        camera.cd().remove_callback(cd_events_cb_id);
        if (em_events_cb_id >= 0)        camera.em().remove_callback(em_events_cb_id);
        if (graylevel_frame_cb_ret >= 0) camera.unset_exposure_frame_callback();

        camera.stop();
    } while (do_retry);

    return 0;
}
