#include <iostream>

#include "bridge/JniBridgePlaceholder.h"
#include "service/SpectraBridgeClient.h"

int main()
{
    spectra::bridge::JniBridgePlaceholder bridge_callbacks;
    spectra::service::SpectraBridgeClient client(&bridge_callbacks);

    // 这里不直接发起连接，后续由 JNI 层按 Java 业务时机调用 Connect / SendXXX / Disconnect。
    std::cout << "SpectraBridge native module is ready." << std::endl;
    (void)client;
    return 0;
}
