#define LOG_TAG "RecordroidTimer"

#define LOG_NDEBUG 0

#include "jni.h"
#include "JNIHelp.h"
#include <utils/Log.h>
#include <utils/misc.h>
#include <android_runtime/AndroidRuntime.h>
#include <android_runtime/Log.h>

#include "android_runtime/AndroidRuntime.h"

#include <stdio.h>
#include <stdlib.h>

static jobject mCallbacksObj = NULL;

namespace android {

static jclass gTimeClass = NULL;
static jmethodID gTimeClassConstructor = NULL;

static void android_recordroid_RecordroidTimer_class_init(JNIEnv* env, jclass clazz)
{
}

static void android_recordroid_RecordroidTimer_init(JNIEnv* env, jobject obj)
{
	if (!mCallbacksObj)
 		mCallbacksObj = env->NewGlobalRef(obj);
}

static jobject android_recordroid_RecordroidTimer_get_uptime(JNIEnv* env, jclass clazz)
{
	static const clockid_t clocks[] = {  
		CLOCK_REALTIME, 
		CLOCK_MONOTONIC, 
		CLOCK_PROCESS_CPUTIME_ID, 
		CLOCK_THREAD_CPUTIME_ID, 
		CLOCK_BOOTTIME 
	};
	struct timespec t; 
	t.tv_sec = t.tv_nsec = 0; 
	clock_gettime(clocks[CLOCK_MONOTONIC], &t); 

	return env->NewObject(gTimeClass, gTimeClassConstructor, (jlong)t.tv_sec, (jlong)t.tv_nsec);
}

static JNINativeMethod sMethods[] = {
	{"native_class_init", "()V", (void *)android_recordroid_RecordroidTimer_class_init},
	{"native_init", "()V", (void *)android_recordroid_RecordroidTimer_init},
	{"native_get_uptime", "()Lcom/android/server/recordroid/Timestamp;", (void *)android_recordroid_RecordroidTimer_get_uptime},
};

int register_android_server_recordroid_RecordroidTimer(JNIEnv* env)
{
	gTimeClass = (jclass)env->NewGlobalRef(env->FindClass("com/android/server/recordroid/Timestamp"));
	gTimeClassConstructor = env->GetMethodID(gTimeClass, "<init>", "(JJ)V");
	return jniRegisterNativeMethods(env, "com/android/server/recordroid/RecordroidTimer", sMethods, NELEM(sMethods));
}
}
