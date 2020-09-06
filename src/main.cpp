#include <chrono>
#include <getopt.h>
#include <iostream>

#include <functional>
#include <wblib/log.h>
#include <wblib/signal_handling.h>
#include <wblib/wbmqtt.h>

#include "adc_driver.h"
#include "config.h"

using namespace std;
using namespace WBMQTT;

TLogger ErrorLogger("ERROR: [wb-adc] ", TLogger::StdErr, TLogger::RED);
TLogger DebugLogger("DEBUG: [wb-adc] ", TLogger::StdErr, TLogger::WHITE, false);
TLogger InfoLogger("INFO: [wb-adc] ", TLogger::StdErr, TLogger::GREY);

namespace
{
    //! Maximum timeout before forced application termination. Topic cleanup can take a lot of time
    const auto DRIVER_STOP_TIMEOUT_S = chrono::seconds(5);

    //! Maximun time to start application. Exceded timeout will case application termination.
    const auto DRIVER_INIT_TIMEOUT_S = chrono::seconds(5);

    void PrintUsage()
    {
        cout << "Usage:" << endl
             << " wb-mqtt-adc [options]" << endl
             << "Options:" << endl
             << "  -d level     enable debuging output:" << endl
             << "                 1 - adc only;" << endl
             << "                 2 - mqtt only;" << endl
             << "                 3 - both;" << endl
             << "                 negative values - silent mode (-1, -2, -3))" << endl
             << "  -c config    config file" << endl
             << "  -p port      MQTT broker port (default: 1883)" << endl
             << "  -h IP        MQTT broker IP (default: localhost)" << endl
             << "  -u user      MQTT user (optional)" << endl
             << "  -P password  MQTT user password (optional)" << endl
             << "  -T prefix    MQTT topic prefix (optional)" << endl;
    }

    void ParseCommadLine(int                   argc,
                         char*                 argv[],
                         TMosquittoMqttConfig& mqttConfig,
                         string&               customConfig)
    {
        int debugLevel = 0;
        int c;

        while ((c = getopt(argc, argv, "d:c:h:p:u:P:T:")) != -1) {
            switch (c) {
            case 'd':
                debugLevel = stoi(optarg);
                break;
            case 'c':
                customConfig = optarg;
                break;
            case 'p':
                mqttConfig.Port = stoi(optarg);
                break;
            case 'h':
                mqttConfig.Host = optarg;
                break;
            case 'T':
                mqttConfig.Prefix = optarg;
                break;
            case 'u':
                mqttConfig.User = optarg;
                break;
            case 'P':
                mqttConfig.Password = optarg;
                break;

            case '?':
            default:
                PrintUsage();
                exit(2);
            }
        }

        switch (debugLevel) {
        case 0:
            break;
        case -1:
            Info.SetEnabled(false);
            break;

        case -2:
            WBMQTT::Info.SetEnabled(false);
            break;

        case -3:
            WBMQTT::Info.SetEnabled(false);
            Info.SetEnabled(false);
            break;

        case 1:
            DebugLogger.SetEnabled(true);
            break;

        case 2:
            WBMQTT::Debug.SetEnabled(true);
            break;

        case 3:
            WBMQTT::Debug.SetEnabled(true);
            DebugLogger.SetEnabled(true);
            break;

        default:
            cout << "Invalid -d parameter value " << debugLevel << endl;
            PrintUsage();
            exit(2);
        }

        if (optind < argc) {
            for (int index = optind; index < argc; ++index) {
                cout << "Skipping unknown argument " << argv[index] << endl;
            }
        }
    }

    void PrintStartupInfo(const TMosquittoMqttConfig& mqttConfig, const string& customConfig)
    {
        cout << "MQTT broker " << mqttConfig.Host << ':' << mqttConfig.Port << endl;
        if (!customConfig.empty()) {
            cout << "Custom config " << customConfig << endl;
        }
    }

} // namespace

int main(int argc, char* argv[])
{
    TMosquittoMqttConfig mqttConfig{};
    mqttConfig.Id = "wb-adc";

    string customConfig;
    ParseCommadLine(argc, argv, mqttConfig, customConfig);
    PrintStartupInfo(mqttConfig, customConfig);

    TPromise<void> initialized;
    SetThreadName("wb-mqtt-adc");
    SignalHandling::Handle({SIGINT, SIGTERM});
    SignalHandling::OnSignals({SIGINT, SIGTERM}, [&] { SignalHandling::Stop(); });

    /* if signal arrived before driver is initialized:
        wait some time to initialize and then exit gracefully
        else if timed out: exit with error
    */
    SignalHandling::SetWaitFor(DRIVER_INIT_TIMEOUT_S, initialized.GetFuture(), [&] {
        ErrorLogger.Log() << "Driver takes too long to initialize. Exiting.";
        cerr << "Error: DRIVER_INIT_TIMEOUT_S" << endl;
        exit(1);
    });

    /* if handling of signal takes too much time: exit with error */
    SignalHandling::SetOnTimeout(DRIVER_STOP_TIMEOUT_S, [&] {
        ErrorLogger.Log() << "Driver takes too long to stop. Exiting.";
        cerr << "Error: DRIVER_STOP_TIMEOUT_S" << endl;
        exit(2);
    });
    SignalHandling::Start();

    try {
        auto mqttDriver =
            NewDriver(TDriverArgs{}
                          .SetBackend(NewDriverBackend(NewMosquittoMqttClient(mqttConfig)))
                          .SetId(mqttConfig.Id)
                          .SetUseStorage(false)
                          .SetReownUnknownDevices(true));

        mqttDriver->StartLoop();
        SignalHandling::OnSignals({SIGINT, SIGTERM}, [&] {
            mqttDriver->StopLoop();
            mqttDriver->Close();
        });

        mqttDriver->WaitForReady();

        TConfig config = LoadConfig("/etc/wb-mqtt-adc.conf",
                                    customConfig,
                                    "/usr/share/wb-mqtt-confed/schemas/wb-mqtt-adc.schema.json");

        if (config.EnableDebugMessages)
            DebugLogger.SetEnabled(true);

        TADCDriver driver(mqttDriver, config, ErrorLogger, DebugLogger, InfoLogger);

        SignalHandling::OnSignals({SIGINT, SIGTERM}, [&] { driver.Stop(); });

        initialized.Complete();
        SignalHandling::Wait();

    } catch (const exception& e) {
        ErrorLogger.Log() << "FATAL: " << e.what();
        SignalHandling::Stop();
        return 1;
    }

    return 0;
}
