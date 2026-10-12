#include <jni.h>

#include <cstdio>

#include "msi_decoder.h"

extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_example_msiscanner_MsiNative_nativeDecodeYPlane(
    JNIEnv* env, jobject, jobject yBuffer, jint offset, jint width, jint height,
    jint rowStride, jint checksumMode, jboolean scanVertical, jint minVotes,
    jint minDigits, jint maxDigits) {
    if (yBuffer == nullptr || width <= 0 || height <= 0 || rowStride < width || offset < 0) {
        return nullptr;
    }
    const auto* base = static_cast<const uint8_t*>(env->GetDirectBufferAddress(yBuffer));
    if (base == nullptr) {
        return nullptr;
    }
    const jlong capacity = env->GetDirectBufferCapacity(yBuffer);
    const jlong needed = static_cast<jlong>(offset) +
        static_cast<jlong>(rowStride) * (height - 1) + width;
    if (capacity >= 0 && capacity < needed) {
        return nullptr;
    }

    msi::Options options;
    options.scanVertical = scanVertical;
    options.minVotes = minVotes;
    options.minDigits = minDigits;
    options.maxDigits = maxDigits;
    const msi::Result result = msi::decodeYPlane(
        base + offset, width, height, rowStride,
        static_cast<msi::ChecksumMode>(checksumMode), options);
    if (!result.found) {
        return nullptr;
    }

    jclass stringClass = env->FindClass("java/lang/String");
    if (stringClass == nullptr) {
        return nullptr;
    }
    jobjectArray array = env->NewObjectArray(3, stringClass, nullptr);
    if (array == nullptr) {
        return nullptr;
    }
    env->SetObjectArrayElement(array, 0, env->NewStringUTF(result.text.c_str()));
    const char* status = "none";
    if (result.checksum == msi::ChecksumStatus::Valid) {
        status = "valid";
    } else if (result.checksum == msi::ChecksumStatus::Invalid) {
        status = "invalid";
    } else if (result.checksum == msi::ChecksumStatus::NotApplicable) {
        status = "n/a";
    }
    env->SetObjectArrayElement(array, 1, env->NewStringUTF(status));
    char meta[160];
    std::snprintf(meta, sizeof(meta), "votes=%d module=%.2f%s%s scheme=%s", result.votes,
                  result.moduleWidth, result.vertical ? " vertical" : "",
                  result.reversed ? " reversed" : "",
                  result.scheme.empty() ? "-" : result.scheme.c_str());
    env->SetObjectArrayElement(array, 2, env->NewStringUTF(meta));
    return array;
}
