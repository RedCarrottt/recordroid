#define LOG_TAG "KernelInputEventPoller"

#define LOG_NDEBUG 0

#define LOG_OVERHEAD 0

#include "jni.h"
#include "JNIHelp.h"
#include <utils/Log.h>
#include <utils/misc.h>
#include <android_runtime/AndroidRuntime.h>
#include <android_runtime/Log.h>

#include "android_runtime/AndroidRuntime.h"

#include <string.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/inotify.h>
#include <sys/limits.h>
#include <sys/poll.h>
#include <linux/input.h>
#include <errno.h>

static jobject mCallbacksObj = NULL;
static jmethodID method_completePoll;

static struct pollfd *ufds;
static char **device_names;
static int nfds;
static long long zero_tv_sec;
static long long zero_tv_usec;

struct raw_input_event_queue_tuple {
	struct timeval time;			// 8B
	unsigned short type;			// 2B
	unsigned short code;			// 2B
	unsigned long value;			// 4B
	unsigned long deviceNum;		// 4B
}; // 20B per a tuple

static const int RAW_INPUT_EVENT_QUEUE_SIZE = 5000;

struct raw_input_event_queue {
	int read_cursor;
	int write_cursor;
	pthread_mutex_t lock;
	struct raw_input_event_queue_tuple tuples[RAW_INPUT_EVENT_QUEUE_SIZE];	// circular queue buffer
};

static struct raw_input_event_queue gRawInputEventQueue;
static bool gIsChunking = false;
static bool gIsPollingWorkerAlive = false;

static inline void init_raw_input_event_queue() {
	struct raw_input_event_queue *queue = &(gRawInputEventQueue);
	queue->read_cursor = 0;
	queue->write_cursor = 0;
	memset(queue->tuples, 0,
		sizeof(struct raw_input_event_queue_tuple) * RAW_INPUT_EVENT_QUEUE_SIZE);
	pthread_mutex_init(&(queue->lock), NULL);
}

static inline void finalize_raw_input_event_queue() {
	// no finalization in raw input event queue
}

static inline struct raw_input_event_queue_tuple* get_raw_input_event_queue_tuple_for_read() {
	return &(gRawInputEventQueue.tuples[gRawInputEventQueue.read_cursor]);
}

static inline struct raw_input_event_queue_tuple* get_raw_input_event_queue_tuple_for_write() {
	return &(gRawInputEventQueue.tuples[gRawInputEventQueue.write_cursor]);
}

static inline void inc_raw_input_event_queue_read_cursor() {
	// It requires lock
	gRawInputEventQueue.read_cursor = (gRawInputEventQueue.read_cursor + 1) % RAW_INPUT_EVENT_QUEUE_SIZE;
}

static inline void inc_raw_input_event_queue_write_cursor() {
	// It requires lock
	gRawInputEventQueue.write_cursor = (gRawInputEventQueue.write_cursor + 1) % RAW_INPUT_EVENT_QUEUE_SIZE;
}

static inline bool is_raw_input_event_queue_writable() {
	// It requires lock
	int read_to_write = ((gRawInputEventQueue.read_cursor - gRawInputEventQueue.write_cursor
		+ RAW_INPUT_EVENT_QUEUE_SIZE) % RAW_INPUT_EVENT_QUEUE_SIZE);
	return (read_to_write > 1 || read_to_write == 0) ? true : false;
}

static inline bool is_raw_input_event_queue_readable(bool isUrgentChunk) {
	// It requires lock
	int write_to_read = ((gRawInputEventQueue.write_cursor - gRawInputEventQueue.read_cursor
		+ RAW_INPUT_EVENT_QUEUE_SIZE) % RAW_INPUT_EVENT_QUEUE_SIZE);
	return ((write_to_read > 1)
		|| ((isUrgentChunk == true) && (write_to_read == 1)))
		? true : false;
}

static inline void lock_raw_input_event_queue() {
	pthread_mutex_lock(&(gRawInputEventQueue.lock));
}

static inline void unlock_raw_input_event_queue() {
	pthread_mutex_unlock(&(gRawInputEventQueue.lock));
}


namespace android {
	static void checkAndClearExceptionFromCallback(JNIEnv* env, const char* methodName) {
		if (env->ExceptionCheck()) {
			ALOGE("An exception was thrown by callback '%s'.", methodName);
			LOGE_EX(env);
			env->ExceptionClear();
		}
	}

	static int open_device(const char *device)
	{
		int version;
		int fd;
		struct pollfd *new_ufds;
		char **new_device_names;
		char name[80];
		char location[80];
		char idstr[80];
		struct input_id id;

		fd = open(device, O_RDWR);
		if(fd < 0) {
			// could not open the device 
			return -10;
		}

		if(ioctl(fd, EVIOCGVERSION, &version)) {
			// could not get driver version for the device 
			return -11;
		}
		if(ioctl(fd, EVIOCGID, &id)) {
			// could not get driver id 
			return -12;
		}
		name[sizeof(name) - 1] = '\0';
		location[sizeof(location) - 1] = '\0';
		idstr[sizeof(idstr) - 1] = '\0';
		if(ioctl(fd, EVIOCGNAME(sizeof(name) - 1), &name) < 1) {
			//fprintf(stderr, "could not get device name for %s, %s\n", device, strerror(errno));
			name[0] = '\0';
		}
		if(ioctl(fd, EVIOCGPHYS(sizeof(location) - 1), &location) < 1) {
			//fprintf(stderr, "could not get location for %s, %s\n", device, strerror(errno));
			location[0] = '\0';
		}
		if(ioctl(fd, EVIOCGUNIQ(sizeof(idstr) - 1), &idstr) < 1) {
			//fprintf(stderr, "could not get idstring for %s, %s\n", device, strerror(errno));
			idstr[0] = '\0';
		}

		new_ufds = (struct pollfd *)realloc(ufds, sizeof(ufds[0]) * (nfds + 1));
		if(new_ufds == NULL) {
			// fprintf(stderr, "out of memory\n");
			return -13;
		}
		ufds = new_ufds;
		new_device_names = (char**)realloc(device_names, sizeof(device_names[0]) * (nfds + 1));
		if(new_device_names == NULL) {
			// fprintf(stderr, "out of memory\n");
			return -14;
		}
		device_names = new_device_names; 

		ufds[nfds].fd = fd;
		ufds[nfds].events = POLLIN;
		device_names[nfds] = strdup(device);
		nfds++;

		return 0;
	}

	static int scan_dir(const char *dirname)
	{
		char devname[PATH_MAX];
		char *filename;
		DIR *dir;
		struct dirent *de;
		dir = opendir(dirname);
		if(dir == NULL)
			return -1;
		strcpy(devname, dirname);
		filename = devname + strlen(devname);
		*filename++ = '/';
		while((de = readdir(dir))) {
			if(de->d_name[0] == '.' &&
					(de->d_name[1] == '\0' ||
					 (de->d_name[1] == '.' && de->d_name[2] == '\0')))
				continue;
			if((strlen(de->d_name) < 6) ||
				(strncmp(de->d_name, "event", strlen(de->d_name)) == 0))
				continue;
			strcpy(filename, de->d_name);
			open_device(devname);
		}
		closedir(dir);
		return 0;
	}

	int close_device(const char *device)
	{
		int i;
		for(i = 1; i < nfds; i++) {
			if(strcmp(device_names[i], device) == 0) {
				int count = nfds - i - 1;
				//remove device
				free(device_names[i]);
				memmove(device_names + i, device_names + i + 1, sizeof(device_names[0]) * count);
				memmove(ufds + i, ufds + i + 1, sizeof(ufds[0]) * count);
				nfds--;
				return 0;
			}
		}
		// remote device not found
		return -20;
	}

	static int read_notify(const char *dirname, int nfd)
	{
		int res;
		char devname[PATH_MAX];
		char *filename;
		char event_buf[512];
		int event_size;
		int event_pos = 0;
		struct inotify_event *event;

		res = read(nfd, event_buf, sizeof(event_buf));
		if(res < (int)sizeof(*event)) {
			if(errno == EINTR)
				return 0;
			fprintf(stderr, "could not get event, %s\n", strerror(errno));
			return 1;
		}
		//got event information

		strcpy(devname, dirname);
		filename = devname + strlen(devname);
		*filename = '/';
		filename++;

		while(res >= (int)sizeof(*event)) {
			event = (struct inotify_event *)(event_buf + event_pos);
			if(event->len) {
				strcpy(filename, event->name);
				if(event->mask & IN_CREATE) {
					open_device(devname);
				}
				else {
					close_device(devname);
				}
			}
			event_size = sizeof(*event) + event->len;
			res -= event_size;
			event_pos += event_size;
		}
		return 0;
	}


	static void android_recordroid_KernelInputEventPoller_class_init(JNIEnv* env, jclass clazz)
	{
		method_completePoll = env->GetMethodID(clazz, "completePoll", "(JJIIII)V");
	}

	static void android_recordroid_KernelInputEventPoller_init(JNIEnv* env, jobject obj)
	{
		if (!mCallbacksObj)
			mCallbacksObj = env->NewGlobalRef(obj);

		// Initialize flags
		gIsChunking = false;
		gIsPollingWorkerAlive = false;

		// Initialize raw input event queue
		init_raw_input_event_queue();
	}

	static void initialize_zero_time()
	{
		const int NSECS_TO_US = 1000;
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
		
		zero_tv_sec = (long long)t.tv_sec;
		zero_tv_usec = (long long)t.tv_nsec / NSECS_TO_US;
	}

	static inline long long get_uptime_microseconds() {
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
		return (((long long)(t.tv_sec) * 1000 * 1000) + (t.tv_nsec / 1000));
	}

	static inline struct timeval get_uptimeval_microseconds() {
		static const clockid_t clocks[] = {  
			CLOCK_REALTIME, 
			CLOCK_MONOTONIC, 
			CLOCK_PROCESS_CPUTIME_ID, 
			CLOCK_THREAD_CPUTIME_ID, 
			CLOCK_BOOTTIME 
		};
		struct timespec t;
		struct timeval ret;
		t.tv_sec = t.tv_nsec = 0; 
		clock_gettime(clocks[CLOCK_MONOTONIC], &t);
		ret.tv_sec = t.tv_sec;
		ret.tv_usec = t.tv_nsec / 1000;
		return ret;
	}

	static jint android_recordroid_KernelInputEventPoller_poll(JNIEnv* env, jobject obj)
	{
		const char *device_path = "/dev/input";
		int res, pollres, i;
		long tv_sec, tv_usec;
		int deviceNum, typeVal, codeVal, value;
		struct input_event event;

		#if LOG_OVERHEAD == 1
		long long initStartUS, initEndUS;
		initStartUS = get_uptime_microseconds();
		#endif

		// Initialize inotify
		nfds = 1;
		ufds = (pollfd*)calloc(1, sizeof(ufds[0]));
		ufds[0].fd = inotify_init();
		ufds[0].events = POLLIN;
		res = inotify_add_watch(ufds[0].fd, device_path, IN_DELETE | IN_CREATE);
		if(res < 0) {
			// could not add watch for the device path
			return -1;
		}
		// Initialize input devices' ids
		res = scan_dir(device_path);
		if(res < 0) {
			// scan dir failed for the device path
			return -2;
		}
		// Initialize zero time
		initialize_zero_time();
		
		#if LOG_OVERHEAD == 1
		initEndUS = get_uptime_microseconds();
		ALOGE("KernelInputEventPoller initialization done %lldus", initEndUS - initStartUS);
		#endif

		while(gIsPollingWorkerAlive == true) {
			// Polling timeout: 1000ms(=1s)
			pollres = poll(ufds, nfds, 1000);
			if(ufds[0].revents & POLLIN) {
				read_notify(device_path, ufds[0].fd);
			}

			struct timeval now = get_uptimeval_microseconds();
			for(i = 1; i < nfds; i++) {
				if(ufds[i].revents) {
					if(ufds[i].revents & POLLIN) {
						#if LOG_OVERHEAD == 1
						long long startUS, compPollStartUS, compPollEndUS, endUS;
						startUS = get_uptime_microseconds();
						#endif
						res = read(ufds[i].fd, &event, sizeof(event));
						if(res < (int)sizeof(event)) {
							// could not get event
							return 1;
						}
						tv_sec = now.tv_sec;
						tv_usec = now.tv_usec;
						typeVal = event.type;
						codeVal = event.code;
						value = event.value;
						deviceNum = (int)((device_names[i])[16] - '0');	// /dev/input/event*

						// Prune events happened before zero time
						if((tv_sec > zero_tv_sec) || 
							((tv_sec == zero_tv_sec) && (tv_usec > zero_tv_usec))) {
							#if LOG_OVERHEAD == 1
							compPollStartUS = get_uptime_microseconds();
							#endif
							// Put the event into raw input event queue
							bool can_write = false;
							do {
								lock_raw_input_event_queue();
								can_write = is_raw_input_event_queue_writable();
								unlock_raw_input_event_queue();
							}while(can_write == false);
							
							raw_input_event_queue_tuple* tuple = get_raw_input_event_queue_tuple_for_write();
							tuple->time.tv_sec = tv_sec;
							tuple->time.tv_usec = tv_usec;
							tuple->type = typeVal;
							tuple->code = codeVal;
							tuple->value= value;
							tuple->deviceNum = deviceNum;

							lock_raw_input_event_queue();
							inc_raw_input_event_queue_write_cursor();
							unlock_raw_input_event_queue();
							
							#if LOG_OVERHEAD == 1
							compPollEndUS = endUS = get_uptime_microseconds();
							
							long long completingInterval = compPollEndUS - compPollStartUS;
							long long totalInterval = endUS - startUS;
							long long headPureInterval = totalInterval - completingInterval;
							// Total = head pure interval + completing interval
							ALOGE("KIE %lld \t%lld \t%lld",
								headPureInterval, completingInterval, totalInterval);
							#endif
						}
					}
				}
			}
		}
		checkAndClearExceptionFromCallback(env, __FUNCTION__);
		return 0;
	}

	static jint android_recordroid_KernelInputEventPoller_chunk(JNIEnv* env, jobject obj,
		jboolean isUrgentChunk)
	{	
		bool is_readable = false;
		bool read_once = false;
		// Make kernel input events one by one
		while(true) {
			lock_raw_input_event_queue();
			is_readable = is_raw_input_event_queue_readable(isUrgentChunk);
			unlock_raw_input_event_queue();

			if(is_readable == false)
				break;

			raw_input_event_queue_tuple* tuple = get_raw_input_event_queue_tuple_for_read();
			long long tv_sec = tuple->time.tv_sec;
			long tv_usec = tuple->time.tv_usec;
			long typeVal = tuple->type;
			long codeVal = tuple->code;
			long value = tuple->value;
			long deviceNum = tuple->deviceNum;
			
			// Complete this poll
			env->CallVoidMethod(mCallbacksObj, 
				method_completePoll, 
				(jlong)tv_sec, (jlong)tv_usec, 
				(jint)deviceNum,
				(jint)typeVal, (jint)codeVal, (jint)value);

			lock_raw_input_event_queue();
			inc_raw_input_event_queue_read_cursor();
			unlock_raw_input_event_queue();

			read_once = true;
		}
		return (read_once == true) ? 0 : -1;
	}

	static void android_recordroid_KernelInputEventPoller_set_is_polling_worker_alive(JNIEnv* env, jobject obj,
		jboolean isPollingWorkerAlive)
	{
		gIsPollingWorkerAlive = (isPollingWorkerAlive == JNI_FALSE) ? false : true;
	}

	static jboolean android_recordroid_KernelInputEventPoller_is_polling_worker_alive(JNIEnv* env, jobject obj)
	{
		return (gIsPollingWorkerAlive == true) ? JNI_TRUE : JNI_FALSE;
	}

	static JNINativeMethod sMethods[] = {
		/* name, signature, funcPtr */
		{"native_class_init", "()V", (void *)android_recordroid_KernelInputEventPoller_class_init},
		{"native_init", "()V", (void *)android_recordroid_KernelInputEventPoller_init},
		{"native_poll", "()I", (void *)android_recordroid_KernelInputEventPoller_poll},	
		{"native_chunk", "(Z)I", (void *)android_recordroid_KernelInputEventPoller_chunk},	
		{"native_set_is_polling_worker_alive", "(Z)V", (void *)android_recordroid_KernelInputEventPoller_set_is_polling_worker_alive},
		{"native_is_polling_worker_alive", "()Z", (void *)android_recordroid_KernelInputEventPoller_is_polling_worker_alive},
	};

	int register_android_server_recordroid_KernelInputEventPoller(JNIEnv* env)
	{
		return jniRegisterNativeMethods(env, "com/android/server/recordroid/KernelInputEventPoller", sMethods, NELEM(sMethods));
	}

}
