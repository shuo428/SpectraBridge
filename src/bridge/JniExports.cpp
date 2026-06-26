#include "bridge/NativeContext.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "protocol/ImageProtocol.h"
#include "springbootjni_jni_SpectraBridgeNative.h"

namespace {

template <typename T>
T* HandleToPointer(jlong handle)
{
    return reinterpret_cast<T*>(static_cast<std::intptr_t>(handle));
}

void ThrowJavaException(JNIEnv* env, const char* class_name, const std::string& message)
{
    jclass exception_class = env->FindClass(class_name);
    if (exception_class == NULL)
    {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return;
    }

    env->ThrowNew(exception_class, message.c_str());
    env->DeleteLocalRef(exception_class);
}

void ThrowIllegalArgument(JNIEnv* env, const std::string& message)
{
    ThrowJavaException(env, "java/lang/IllegalArgumentException", message);
}

void ThrowIllegalState(JNIEnv* env, const std::string& message)
{
    ThrowJavaException(env, "java/lang/IllegalStateException", message);
}

void ThrowRuntimeError(JNIEnv* env, const std::string& message)
{
    ThrowJavaException(env, "java/lang/RuntimeException", message);
}

spectra::bridge::NativeContext* RequireContext(JNIEnv* env, jlong native_handle)
{
    spectra::bridge::NativeContext* context = HandleToPointer<spectra::bridge::NativeContext>(native_handle);
    if (context == NULL)
    {
        ThrowIllegalState(env, "native handle is null or already destroyed");
    }
    return context;
}

std::string JStringToUtf8(JNIEnv* env, jstring value, bool* ok)
{
    *ok = false;

    if (value == NULL)
    {
        return std::string();
    }

    const char* chars = env->GetStringUTFChars(value, NULL);
    if (chars == NULL)
    {
        return std::string();
    }

    const std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    *ok = true;
    return result;
}

bool IsValidPort(jint port)
{
    return port >= 1 && port <= 65535;
}

}  // namespace

JNIEXPORT jlong JNICALL Java_springbootjni_jni_SpectraBridgeNative_nativeCreate(
    JNIEnv* env,
    jclass,
    jobject listener)
{
    std::string error;
    std::unique_ptr<spectra::bridge::JniBridge> bridge =
        spectra::bridge::JniBridge::Create(env, listener, &error);
    if (!bridge)
    {
        ThrowIllegalState(env, error.empty() ? "failed to create JNI bridge" : error);
        return 0;
    }

    std::unique_ptr<spectra::bridge::NativeContext> context(
        new spectra::bridge::NativeContext(std::move(bridge)));
    return static_cast<jlong>(reinterpret_cast<std::intptr_t>(context.release()));
}

JNIEXPORT void JNICALL Java_springbootjni_jni_SpectraBridgeNative_nativeDestroy(
    JNIEnv* env,
    jclass,
    jlong native_handle)
{
    if (native_handle == 0)
    {
        return;
    }

    std::unique_ptr<spectra::bridge::NativeContext> context(
        HandleToPointer<spectra::bridge::NativeContext>(native_handle));
    if (!context)
    {
        ThrowIllegalState(env, "native handle is null or already destroyed");
        return;
    }

    context->client->Disconnect();
}

JNIEXPORT void JNICALL Java_springbootjni_jni_SpectraBridgeNative_nativeConnect(
    JNIEnv* env,
    jclass,
    jlong native_handle,
    jstring host,
    jint control_port,
    jint image_port,
    jboolean verify_crc)
{
    spectra::bridge::NativeContext* context = RequireContext(env, native_handle);
    if (context == NULL)
    {
        return;
    }

    if (host == NULL)
    {
        ThrowIllegalArgument(env, "host must not be null");
        return;
    }

    if (!IsValidPort(control_port) || !IsValidPort(image_port))
    {
        ThrowIllegalArgument(env, "controlPort and imagePort must be in range 1..65535");
        return;
    }

    bool ok = false;
    const std::string host_value = JStringToUtf8(env, host, &ok);
    if (!ok)
    {
        ThrowRuntimeError(env, "failed to convert host string from Java to UTF-8");
        return;
    }

    spectra::service::SpectraBridgeConfig config;
    config.control_endpoint.host = host_value;
    config.control_endpoint.port = static_cast<uint16_t>(control_port);
    config.image_endpoint.host = host_value;
    config.image_endpoint.port = static_cast<uint16_t>(image_port);
    config.verify_crc = (verify_crc == JNI_TRUE);
    config.expected_width = 800u;
    config.expected_height = 600u;
    config.expected_pixel_format = spectra::protocol::kPixelFormatRaw16Low12;

    std::string error;
    if (!context->client->Connect(config, &error))
    {
        ThrowRuntimeError(env, error.empty() ? "native connect failed" : error);
    }
}

JNIEXPORT void JNICALL Java_springbootjni_jni_SpectraBridgeNative_nativeDisconnect(
    JNIEnv* env,
    jclass,
    jlong native_handle)
{
    spectra::bridge::NativeContext* context = RequireContext(env, native_handle);
    if (context == NULL)
    {
        return;
    }

    context->client->Disconnect();
}

JNIEXPORT void JNICALL Java_springbootjni_jni_SpectraBridgeNative_nativeSendReset(
    JNIEnv* env,
    jclass,
    jlong native_handle)
{
    spectra::bridge::NativeContext* context = RequireContext(env, native_handle);
    if (context == NULL)
    {
        return;
    }

    std::string error;
    if (!context->client->SendReset(&error))
    {
        ThrowRuntimeError(env, error.empty() ? "native sendReset failed" : error);
    }
}

JNIEXPORT void JNICALL Java_springbootjni_jni_SpectraBridgeNative_nativeSendTriggerOnce(
    JNIEnv* env,
    jclass,
    jlong native_handle)
{
    spectra::bridge::NativeContext* context = RequireContext(env, native_handle);
    if (context == NULL)
    {
        return;
    }

    std::string error;
    if (!context->client->SendTriggerOnce(&error))
    {
        ThrowRuntimeError(env, error.empty() ? "native sendTriggerOnce failed" : error);
    }
}

JNIEXPORT void JNICALL Java_springbootjni_jni_SpectraBridgeNative_nativeSendQueryStatus(
    JNIEnv* env,
    jclass,
    jlong native_handle)
{
    spectra::bridge::NativeContext* context = RequireContext(env, native_handle);
    if (context == NULL)
    {
        return;
    }

    std::string error;
    if (!context->client->SendQueryStatus(&error))
    {
        ThrowRuntimeError(env, error.empty() ? "native sendQueryStatus failed" : error);
    }
}

JNIEXPORT void JNICALL Java_springbootjni_jni_SpectraBridgeNative_nativeSendFullConfig(
    JNIEnv* env,
    jclass,
    jlong native_handle,
    jbyteArray regs_512)
{
    spectra::bridge::NativeContext* context = RequireContext(env, native_handle);
    if (context == NULL)
    {
        return;
    }

    if (regs_512 == NULL)
    {
        ThrowIllegalArgument(env, "regs512 must not be null");
        return;
    }

    const jsize array_length = env->GetArrayLength(regs_512);
    if (array_length != static_cast<jsize>(springbootjni_jni_SpectraBridgeNative_FULL_CONFIG_SIZE))
    {
        ThrowIllegalArgument(env, "regs512 length must be exactly 512 bytes");
        return;
    }

    std::array<uint8_t, spectra::protocol::kFullConfigPayloadSize> regs = {};
    env->GetByteArrayRegion(regs_512,
                            0,
                            array_length,
                            reinterpret_cast<jbyte*>(regs.data()));
    if (env->ExceptionCheck())
    {
        return;
    }

    std::string error;
    if (!context->client->SendFullConfig(regs, &error))
    {
        ThrowRuntimeError(env, error.empty() ? "native sendFullConfig failed" : error);
    }
}
