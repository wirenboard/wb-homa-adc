#include <string>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <cmath>
#include <ctime>
#include <unistd.h>
#include <chrono>
#include <thread>
#include <math.h>
#include <sys/types.h>
#include <dirent.h>
#include <string.h>

#include <fnmatch.h>

#include "sysfs_adc.h"
#include "lradc_isrc.h"
#include "wbmqtt/utils.h"
namespace {
        extern "C" void usleep(int value);
};

int TryOpen(std::vector<std::string> fnames, std::ifstream& file)
{
    for (auto& fname : fnames) {
        file.close();
        file.clear();
        file.open(fname);
        if (file.is_open()) {
            return 0;
        }
    }
    return -1;
}

void TSysfsAdc::SelectScale()
{
    std::string scale_prefix = SysfsIIODir + "/in_" + GetLradcChannel() + "_scale";

    std::ifstream scale_file("/dev/null");
    TryOpen({ scale_prefix + "_available", 
              SysfsIIODir + "/in_voltage_scale_available",
              SysfsIIODir + "/scale_available"
            }, scale_file);

    // read list of available scales
    if (scale_file.is_open()) {
        auto contents = std::string((std::istreambuf_iterator<char>(scale_file)), std::istreambuf_iterator<char>());
        auto available_scale_strs = StringSplit(contents, " ");

        string best_scale_str;
        double best_scale = 0;

        for (auto& scale_str : available_scale_strs) {
            double val;
            try {
                val = stod(scale_str);
            } catch (std::invalid_argument e) {
                continue;
            }
            // best scale is either maximum scale or the one closest to user request
            if (((ChannelConfig.Scale > 0) && (fabs(val - ChannelConfig.Scale) <= fabs(best_scale - ChannelConfig.Scale)))      // user request
                ||
                ((ChannelConfig.Scale <= 0) && (val >= best_scale))      // maximum scale
                )
            {
                best_scale = val;
                best_scale_str = scale_str;
            }
        
        }

        scale_file.close();
        IIOScale = best_scale;

        ofstream write_scale(scale_prefix);
        if (!write_scale.is_open()) {
            throw TAdcException("error opening IIO sysfs scale file");
        }
        write_scale << best_scale_str;
        write_scale.close();
    } else {
        // scale_available file is not present
        // read the current scale from sysfs
        ifstream cur_scale(scale_prefix);
        if (cur_scale.is_open()) {
            cur_scale >> IIOScale;
            cur_scale.close();
        } else {
            // if in_voltageX_scale file is not available, try to read group scale,
            //  i.e. in_voltage_scale
            ifstream cur_group_scale(SysfsIIODir + "/in_voltage_scale");
            if (cur_group_scale.is_open()) {
                cur_group_scale >> IIOScale;
                cur_group_scale.close();
            }
        }
    }
}


TSysfsAdc::TSysfsAdc(const std::string& sysfs_dir, bool debug, const TChannel& channel_config)
    : SysfsDir(sysfs_dir),
    ChannelConfig(channel_config),
    MaxVoltage(ChannelConfig.MaxVoltage)
{
    IIOScale = MXS_LRADC_DEFAULT_SCALE_FACTOR;
    AveragingWindow = ChannelConfig.AveragingWindow;
    Debug = debug;
    Initialized = false;

    string iio_dev_dir = SysfsDir + "/bus/iio/devices";
    string iio_dev_name = "";
    if (!channel_config.MatchIIO.empty()) {
        string Pattern = "*" + channel_config.MatchIIO + "*";
        DIR *dir;
        struct dirent *ent;
        if ((dir = opendir(iio_dev_dir.c_str())) != NULL) {
            while ((ent = readdir (dir)) != NULL) {
                if (!strstr(ent->d_name, "iio:device"))
                    continue;
                string d = iio_dev_dir + "/" + string(ent->d_name);
                char buf[512];
                int len;
                if ((len = readlink(d.c_str(), buf, 512)) < 0)
                    continue;
                buf[len] = 0;

                // POSIX shell-like matching
                if (fnmatch(Pattern.c_str(), buf, 0) == 0) {
                    iio_dev_name = string(ent->d_name);
                    break;
                }
            }
            closedir(dir);
            if (iio_dev_name.empty()) {
                throw TAdcException("couldn't match sysfs IIO " + channel_config.MatchIIO);
            }
        }
        else {
            /* could not open directory */
            throw TAdcException("error opening sysfs IIO directory");
        }
    }
    else {
        iio_dev_name = "iio:device0";
    }
    SysfsIIODir = iio_dev_dir + "/" + iio_dev_name;

    string path_to_value = SysfsIIODir + "/in_" + GetLradcChannel() + "_raw";
    AdcValStream.open(path_to_value);
    if (!AdcValStream.is_open()) {
        throw TAdcException("error opening sysfs Adc file");
    }
    
    SelectScale();

     ::SwitchOffCurrentSource(
        GetCurrentSourceChannelNumber(
            GetLradcChannel()));

    NumberOfChannels = channel_config.Mux.size();
}

std::unique_ptr<TSysfsAdcChannel> TSysfsAdc::GetChannel(int i)
{
    
    // Check whether all Mux channels are OHM_METERs
    bool resistance_channels_only = true;
    for (const auto & mux_ch : ChannelConfig.Mux) {
        if (mux_ch.Type != OHM_METER) {
            resistance_channels_only = false;
        }
    }    
        
    // TBD: should pass chain_alias also (to be used instead of Name for the channel)
    if (ChannelConfig.Mux[i].Type == OHM_METER)
        return std::unique_ptr<TSysfsAdcChannelRes>(new TSysfsAdcChannelRes(this, ChannelConfig.Mux[i].MuxChannelNumber, ChannelConfig.Mux[i].Id, 
                                           ChannelConfig.Mux[i].ReadingsNumber, ChannelConfig.Mux[i].DecimalPlaces, 
                                           ChannelConfig.Mux[i].DischargeChannel, ChannelConfig.Mux[i].Current, 
                                           ChannelConfig.Mux[i].Resistance1, ChannelConfig.Mux[i].Resistance2,
                                           /* current_source_always_on = */ resistance_channels_only,
                                           ChannelConfig.Mux[i].CurrentCalibrationFactor,
                                           ChannelConfig.Mux[i].MqttType
                                           ));
    else
        return  std::unique_ptr<TSysfsAdcChannel>(new TSysfsAdcChannel(this, ChannelConfig.Mux[i].MuxChannelNumber,
                                       ChannelConfig.Mux[i].Id, ChannelConfig.Mux[i].ReadingsNumber,
                                       ChannelConfig.Mux[i].DecimalPlaces, ChannelConfig.Mux[i].DischargeChannel,
                                       ChannelConfig.Mux[i].MqttType,
                                       ChannelConfig.Mux[i].Multiplier));
    return std::unique_ptr<TSysfsAdcChannel>(nullptr);
}

int TSysfsAdc::ReadValue()
{
    int val;
    AdcValStream.seekg(0);
    AdcValStream >> val;
    return val;
}

bool TSysfsAdc::CheckVoltage(int value)
{
    float voltage = IIOScale * value;
    if (voltage > MaxVoltage) {
        return false;
    }
    return true;
}


TSysfsAdcMux::TSysfsAdcMux(const std::string& sysfs_dir, bool debug, const TChannel& channel_config)
    : TSysfsAdc(sysfs_dir, debug, channel_config)
{
    MinSwitchIntervalMs = ChannelConfig.MinSwitchIntervalMs;
    CurrentMuxInput = -1;
    GpioMuxA = ChannelConfig.Gpios[0];
    GpioMuxB = ChannelConfig.Gpios[1];
    GpioMuxC = ChannelConfig.Gpios[2];
}


void TSysfsAdcMux::SelectMuxChannel(int index)
{
    SetMuxABC(index);
}

void TSysfsAdcMux::InitMux()
{
    if (Initialized)
        return;
    InitGPIO(GpioMuxA);
    InitGPIO(GpioMuxB);
    InitGPIO(GpioMuxC);
    Initialized = true;
}

void TSysfsAdcMux::InitGPIO(int gpio)
{
    std::string gpio_direction_path = GPIOPath(gpio, "/direction");
    std::ofstream setdirgpio(gpio_direction_path);
    if (!setdirgpio) {
        std::ofstream exportgpio(SysfsDir + "/class/gpio/export");
        if (!exportgpio)
            throw TAdcException("unable to export GPIO " + std::to_string(gpio));
        exportgpio << gpio << std::endl;
        setdirgpio.clear();
        setdirgpio.open(gpio_direction_path);
        if (!setdirgpio)
            throw TAdcException("unable to set GPIO direction" + std::to_string(gpio));
    }
    setdirgpio << "out";
}

void TSysfsAdcMux::SetGPIOValue(int gpio, int value)
{
    std::ofstream setvalgpio(GPIOPath(gpio, "/value"));
    if (!setvalgpio)
        throw TAdcException("unable to set value of gpio " + std::to_string(gpio));
    setvalgpio << value << std::endl;
}

std::string TSysfsAdcMux::GPIOPath(int gpio, const std::string& suffix) const
{
    return std::string(SysfsDir + "/class/gpio/gpio") + std::to_string(gpio) + suffix;
}

void TSysfsAdcMux::MaybeWaitBeforeSwitching()
{
    if (MinSwitchIntervalMs <= 0)
        return;

    struct timespec tp;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &tp) < 0)
        throw TAdcException("unable to get timer value");

    if (CurrentMuxInput >= 0) { // no delays before the first switch
        double elapsed_ms = (tp.tv_sec - PrevSwitchTS.tv_sec) * 1000 +
            (tp.tv_nsec - PrevSwitchTS.tv_nsec) / 1000000;
        if (Debug)
            std::cerr << "elapsed: " << elapsed_ms << std::endl;
        if (elapsed_ms < MinSwitchIntervalMs) {
            if (Debug)
                std::cerr << "usleep: " << (MinSwitchIntervalMs - (int)elapsed_ms) * 1000 <<
                    std::endl;
            usleep((MinSwitchIntervalMs - (int)elapsed_ms) * 1000);
        }
    }
    PrevSwitchTS = tp;
}

void TSysfsAdcMux::SetMuxABC(int n)
{
    InitMux();
    if (CurrentMuxInput == n)
        return;
    if (Debug)
        std::cerr << "SetMuxABC: " << n << std::endl;
    SetGPIOValue(GpioMuxA, n & 1);
    SetGPIOValue(GpioMuxB, n & 2);
    SetGPIOValue(GpioMuxC, n & 4);
    CurrentMuxInput = n;
    usleep(MinSwitchIntervalMs * 1000);
}

TSysfsAdcPhys::TSysfsAdcPhys(const std::string& sysfs_dir, bool debug, const TChannel& channel_config)
    : TSysfsAdc(sysfs_dir, debug, channel_config)
{
}

void TSysfsAdcPhys::SelectMuxChannel(int index)
{
}


TSysfsAdcChannel::TSysfsAdcChannel(TSysfsAdc* owner, int index, const std::string& name, int readings_number, int decimal_places, int discharge_channel, std::string mqtt_type, float multiplier)
    : DecimalPlaces(decimal_places)
    , d(new TSysfsAdcChannelPrivate())
    , Multiplier(multiplier)
    , MqttType(mqtt_type)
{
    d->Owner = owner;
    d->Index = index;
    d->Name = name;
    d->ReadingsNumber = readings_number;
    d->ChannelAveragingWindow = readings_number * d->Owner->AveragingWindow;
    d->Buffer.resize(d->ChannelAveragingWindow); // initializes with zeros
    d->DischargeChannel = discharge_channel;
}

int TSysfsAdcChannel::GetAverageValue()
{
    if (!d->Ready) {
        for (int i = 0; i < d->ChannelAveragingWindow; ++i) {
            d->Owner->SelectMuxChannel(d->Index);
            int v = d->Owner->ReadValue();

            d->Buffer[i] = v;
            d->Sum += v;
        }
        d->Ready = true;
    } else {
        for (int i = 0; i < d->ReadingsNumber; i++) {
            d->Owner->SelectMuxChannel(d->Index);
            int v = d->Owner->ReadValue();
            d->Sum -= d->Buffer[d->Pos];
            d->Sum += v;
            d->Buffer[d->Pos++] = v;
            d->Pos %= d->ChannelAveragingWindow;
            this_thread::sleep_for(chrono::milliseconds(DELAY));
        }
    }
    return round(d->Sum / d->ChannelAveragingWindow);
}

const std::string& TSysfsAdcChannel::GetName() const
{
    return d->Name;
}

float TSysfsAdcChannel::GetValue()
{
    float result = std::nan("");
    int value = GetAverageValue();
    if (value < ADC_VALUE_MAX) {
        if (d->Owner->CheckVoltage(value)) {
            result = (float) value * Multiplier / 1000; // set voltage to V from mV
            result *= d->Owner->IIOScale;
        }
    }

    return result;
}
std::string TSysfsAdcChannel::GetType()
{
    return MqttType.empty() ? GetDefaultMqttType() : MqttType;
}

std::string TSysfsAdcChannel::GetDefaultMqttType()
{
    return "voltage";
}

TSysfsAdcChannelRes::TSysfsAdcChannelRes(TSysfsAdc* owner, int index, const std::string& name,
                                         int readings_number, int decimal_places, int discharge_channel, 
                                         int current, int resistance1, int resistance2, 
                                         bool source_always_on, float current_calibration_factor, 
                                         std::string mqtt_type)
    : TSysfsAdcChannel(owner, index, name, readings_number, decimal_places, discharge_channel, mqtt_type)
    , Current(current)
    , Resistance1(resistance1)
    , Resistance2(resistance2)
    , Type(OHM_METER)
    , SourceAlwaysOn(source_always_on)
    , CurrentCalibrationFactor(current_calibration_factor)
{
    CurrentSourceChannel =  GetCurrentSourceChannelNumber(owner->GetLradcChannel());

    if (SourceAlwaysOn) SetUpCurrentSource(); 
}


float TSysfsAdcChannelRes::GetValue()
{
    if (d->DischargeChannel != -1) {
        d->Owner->SelectMuxChannel(d->DischargeChannel);
    }
    d->Owner->SelectMuxChannel(d->Index);
    
    if (!SourceAlwaysOn) {
        SetUpCurrentSource(); 
        this_thread::sleep_for(chrono::milliseconds(DELAY));
    }
    
    int value = GetAverageValue(); 
    float result = std::nan("");
    if (value < ADC_VALUE_MAX) {
        if (d->Owner->CheckVoltage(value)) {
            float voltage = d->Owner->IIOScale * value / 1000;// get voltage in V (from mV)
            result = 1.0/ ((Current * CurrentCalibrationFactor / 1000000.0) / voltage - 1.0/Resistance1) - Resistance2;
            if (result < 0) {
                result = 0;
            }
            result = round(result);
        }
    }
    
    if (!SourceAlwaysOn) SwitchOffCurrentSource();
    return result;
}

std::string TSysfsAdcChannelRes::GetDefaultMqttType()
{
    return "resistance";
}
void TSysfsAdcChannelRes::SetUpCurrentSource()
{
    ::SetUpCurrentSource(CurrentSourceChannel, Current);
}

void TSysfsAdcChannelRes::SwitchOffCurrentSource()
{
    ::SwitchOffCurrentSource(CurrentSourceChannel);
}
