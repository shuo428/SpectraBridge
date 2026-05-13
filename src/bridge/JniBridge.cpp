#include "bridge/JniBridge.h"

#include <vector>

namespace spectra {
namespace bridge {

namespace {

const char* kOnImageFrameMethodName = "onImageFrame";
const char* kOnImageFrameMethodSignature = "(II[S[B)V";
const char* kOnStatusMethodName = "onStatus";
const char* kOnStatusMethodSignature = "(II)V";
const char* kOnConfigAckMethodName = "onConfigAck";
const char* kOnConfigAckMethodSignature = "(II)V";
const char* kOnTransportErrorMethodName = "onTransportError";
const char* kOnTransportErrorMethodSignature = "(Ljava/lang/String;Ljava/lang/String;)V";

}  // namespace

std::unique_ptr<JniBridge> JniBridge::Create(JNIEnv* env, jobject listener, std::string* error)
{
    if (listener == NULL)
    {
        if (error != NULL)
        {
            *error = "BridgeListener must not be null";
        }
        return std::unique_ptr<JniBridge>();
    }

    JavaVM* java_vm = NULL;
    if (env->GetJavaVM(&java_vm) != JNI_OK || java_vm == NULL)
    {
        if (error != NULL)
        {
            *error = "failed to obtain JavaVM from JNIEnv";
        }
        return std::unique_ptr<JniBridge>();
    }

    std::unique_ptr<JniBridge> bridge(new JniBridge(java_vm));
    if (!bridge->Initialize(env, listener, error))
    {
        return std::unique_ptr<JniBridge>();
    }

    return bridge;
}

JniBridge::JniBridge(JavaVM* java_vm)
    : java_vm_(java_vm),
      listener_global_ref_(NULL),
      on_image_frame_method_(NULL),
      on_status_method_(NULL),
      on_config_ack_method_(NULL),
      on_transport_error_method_(NULL)
{
}

JniBridge::~JniBridge()
{
    if (listener_global_ref_ == NULL)
    {
        return;
    }

    JNIEnv* env = NULL;
    bool did_attach = false;
    std::string error;
    if (!AcquireThreadEnv(&env, &did_attach, &error))
    {
        return;
    }

    env->DeleteGlobalRef(listener_global_ref_);
    listener_global_ref_ = NULL;

    ReleaseThreadEnv(did_attach);
}

bool JniBridge::Initialize(JNIEnv* env, jobject listener, std::string* error)
{
    listener_global_ref_ = env->NewGlobalRef(listener);
    if (listener_global_ref_ == NULL)
    {
        if (error != NULL)
        {
            *error = "failed to create global reference for BridgeListener";
        }
        return false;
    }

    jclass listener_class = env->GetObjectClass(listener);
    if (listener_class == NULL)
    {
        if (error != NULL)
        {
            *error = "failed to resolve BridgeListener runtime class";
        }
        return false;
    }

    on_image_frame_method_ =
        env->GetMethodID(listener_class, kOnImageFrameMethodName, kOnImageFrameMethodSignature);
    on_status_method_ =
        env->GetMethodID(listener_class, kOnStatusMethodName, kOnStatusMethodSignature);
    on_config_ack_method_ =
        env->GetMethodID(listener_class, kOnConfigAckMethodName, kOnConfigAckMethodSignature);
    on_transport_error_method_ =
        env->GetMethodID(listener_class, kOnTransportErrorMethodName, kOnTransportErrorMethodSignature);

    env->DeleteLocalRef(listener_class);

    if (on_image_frame_method_ == NULL ||
        on_status_method_ == NULL ||
        on_config_ack_method_ == NULL ||
        on_transport_error_method_ == NULL)
    {
        LogAndClearJavaException(env);
        if (error != NULL)
        {
            *error = "failed to resolve one or more BridgeListener callback methods";
        }
        return false;
    }

    return true;
}

bool JniBridge::AcquireThreadEnv(JNIEnv** env, bool* did_attach, std::string* error) const
{
    *env = NULL;
    *did_attach = false;

    const jint get_env_result = java_vm_->GetEnv(reinterpret_cast<void**>(env), JNI_VERSION_1_6);
    if (get_env_result == JNI_OK)
    {
        return true;
    }

    if (get_env_result != JNI_EDETACHED)
    {
        if (error != NULL)
        {
            *error = "JavaVM::GetEnv returned an unexpected result";
        }
        return false;
    }

#ifdef _WIN32
    if (java_vm_->AttachCurrentThread(reinterpret_cast<void**>(env), NULL) != JNI_OK)
#else
    if (java_vm_->AttachCurrentThread(reinterpret_cast<JNIEnv**>(env), NULL) != JNI_OK)
#endif
    {
        if (error != NULL)
        {
            *error = "AttachCurrentThread failed";
        }
        return false;
    }

    *did_attach = true;
    return true;
}

void JniBridge::ReleaseThreadEnv(bool did_attach) const
{
    if (did_attach)
    {
        java_vm_->DetachCurrentThread();
    }
}

void JniBridge::LogAndClearJavaException(JNIEnv* env)
{
    if (env->ExceptionCheck())
    {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

void JniBridge::OnImageFrameReady(const image::ConvertedImageFrame& frame)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);

    JNIEnv* env = NULL;
    bool did_attach = false;
    std::string error;
    if (!AcquireThreadEnv(&env, &did_attach, &error))
    {
        return;
    }

    const jsize pixel_count = static_cast<jsize>(frame.pixels16.size());

    jshortArray pixels16_array = env->NewShortArray(pixel_count);
    if (pixels16_array == NULL)
    {
        LogAndClearJavaException(env);
        ReleaseThreadEnv(did_attach);
        return;
    }

    std::vector<jshort> pixels16_values(pixel_count, 0);
    for (jsize index = 0; index < pixel_count; ++index)
    {
        pixels16_values[index] = static_cast<jshort>(frame.pixels16[static_cast<std::size_t>(index)]);
    }
    env->SetShortArrayRegion(pixels16_array, 0, pixel_count, pixels16_values.data());
    if (env->ExceptionCheck())
    {
        LogAndClearJavaException(env);
        env->DeleteLocalRef(pixels16_array);
        ReleaseThreadEnv(did_attach);
        return;
    }

    jbyteArray pixels8_array = env->NewByteArray(static_cast<jsize>(frame.pixels8.size()));
    if (pixels8_array == NULL)
    {
        LogAndClearJavaException(env);
        env->DeleteLocalRef(pixels16_array);
        ReleaseThreadEnv(did_attach);
        return;
    }

    env->SetByteArrayRegion(pixels8_array,
                            0,
                            static_cast<jsize>(frame.pixels8.size()),
                            reinterpret_cast<const jbyte*>(frame.pixels8.data()));
    if (env->ExceptionCheck())
    {
        LogAndClearJavaException(env);
        env->DeleteLocalRef(pixels16_array);
        env->DeleteLocalRef(pixels8_array);
        ReleaseThreadEnv(did_attach);
        return;
    }

    env->CallVoidMethod(listener_global_ref_,
                        on_image_frame_method_,
                        static_cast<jint>(frame.width),
                        static_cast<jint>(frame.height),
                        pixels16_array,
                        pixels8_array);
    LogAndClearJavaException(env);

    env->DeleteLocalRef(pixels16_array);
    env->DeleteLocalRef(pixels8_array);
    ReleaseThreadEnv(did_attach);
}

void JniBridge::OnStatusPacket(const protocol::StatusPacket& packet)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);

    JNIEnv* env = NULL;
    bool did_attach = false;
    std::string error;
    if (!AcquireThreadEnv(&env, &did_attach, &error))
    {
        return;
    }

    env->CallVoidMethod(listener_global_ref_,
                        on_status_method_,
                        static_cast<jint>(packet.status_bits),
                        static_cast<jint>(packet.error_code));
    LogAndClearJavaException(env);
    ReleaseThreadEnv(did_attach);
}

void JniBridge::OnConfigAckPacket(const protocol::ConfigAckPacket& packet)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);

    JNIEnv* env = NULL;
    bool did_attach = false;
    std::string error;
    if (!AcquireThreadEnv(&env, &did_attach, &error))
    {
        return;
    }

    env->CallVoidMethod(listener_global_ref_,
                        on_config_ack_method_,
                        static_cast<jint>(packet.result_code),
                        static_cast<jint>(packet.failed_addr));
    LogAndClearJavaException(env);
    ReleaseThreadEnv(did_attach);
}

void JniBridge::OnTransportError(const std::string& channel, const std::string& message)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);

    JNIEnv* env = NULL;
    bool did_attach = false;
    std::string error;
    if (!AcquireThreadEnv(&env, &did_attach, &error))
    {
        return;
    }

    jstring channel_string = env->NewStringUTF(channel.c_str());
    if (channel_string == NULL)
    {
        LogAndClearJavaException(env);
        ReleaseThreadEnv(did_attach);
        return;
    }

    jstring message_string = env->NewStringUTF(message.c_str());
    if (message_string == NULL)
    {
        LogAndClearJavaException(env);
        env->DeleteLocalRef(channel_string);
        ReleaseThreadEnv(did_attach);
        return;
    }

    env->CallVoidMethod(listener_global_ref_,
                        on_transport_error_method_,
                        channel_string,
                        message_string);
    LogAndClearJavaException(env);

    env->DeleteLocalRef(channel_string);
    env->DeleteLocalRef(message_string);
    ReleaseThreadEnv(did_attach);
}

}  // namespace bridge
}  // namespace spectra
