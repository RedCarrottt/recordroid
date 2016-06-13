#define LOG_TAG "EventReplayer"

#define LOG_NDEBUG 0

#define LOG_OVERHEAD 0

#include "JNIHelp.h"
#include "jni.h"
#include <utils/Log.h>
#include <utils/misc.h>
#include <android_runtime/AndroidRuntime.h>
#include <android_runtime/Log.h>

#include "android_runtime/AndroidRuntime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

struct input_event {
	struct timeval time;
	unsigned short type;
	unsigned short code;
	unsigned long value;
};

// from <linux/input.h>
#define EVIOCGVERSION		_IOR('E', 0x01, int)			/* get driver version */
#define EVIOCGID		_IOR('E', 0x02, struct input_id)	/* get device ID */
#define EVIOCGKEYCODE		_IOR('E', 0x04, int[2])			/* get keycode */
#define EVIOCSKEYCODE		_IOW('E', 0x04, int[2])			/* set keycode */

#define EVIOCGNAME(len)		_IOC(_IOC_READ, 'E', 0x06, len)		/* get device name */
#define EVIOCGPHYS(len)		_IOC(_IOC_READ, 'E', 0x07, len)		/* get physical location */
#define EVIOCGUNIQ(len)		_IOC(_IOC_READ, 'E', 0x08, len)		/* get unique identifier */

#define EVIOCGKEY(len)		_IOC(_IOC_READ, 'E', 0x18, len)		/* get global keystate */
#define EVIOCGLED(len)		_IOC(_IOC_READ, 'E', 0x19, len)		/* get all LEDs */
#define EVIOCGSND(len)		_IOC(_IOC_READ, 'E', 0x1a, len)		/* get all sounds status */
#define EVIOCGSW(len)		_IOC(_IOC_READ, 'E', 0x1b, len)		/* get all switch states */

#define EVIOCGBIT(ev,len)	_IOC(_IOC_READ, 'E', 0x20 + ev, len)	/* get event bits */
#define EVIOCGABS(abs)		_IOR('E', 0x40 + abs, struct input_absinfo)		/* get abs value/limits */
#define EVIOCSABS(abs)		_IOW('E', 0xc0 + abs, struct input_absinfo)		/* set abs value/limits */

#define EVIOCSFF		_IOC(_IOC_WRITE, 'E', 0x80, sizeof(struct ff_effect))	/* send a force effect to a force feedback device */
#define EVIOCRMFF		_IOW('E', 0x81, int)			/* Erase a force effect */
#define EVIOCGEFFECTS		_IOR('E', 0x84, int)			/* Report number of effects playable at the same time */

#define EVIOCGRAB		_IOW('E', 0x90, int)			/* Grab/Release device */
// end <linux/input.h>

#define ARRAYSIZE(x)  (sizeof(x)/sizeof(*(x)))


struct kernel_input_event {
	long deviceNum;					// 4B
	long typeVal;					// 4B
	long codeVal;					// 4B
	long value;						// 4B
	// 16B per an event
};
struct platform_event {
	long peType;					// 4B
	long response_time_us;			// 4B
	long priv;						// 4B
	long second_priv;			// 4B
	// 16B per an event
};

struct replay_buffer_tuple {
	long long timestamp;	// 8B
	long eventType;			// 4B
	union {
		struct kernel_input_event ke;
		struct platform_event pe;
	} prv;				// 16B
	// 28B per a tuple
};

struct replay_buffer {
	long long sn;
	int size;
	int cursor;
	pthread_mutex_t lock;
	struct replay_buffer_tuple* fixed_tuples;	// fixed size
	struct replay_buffer_tuple* extra_tuples;	// flexible size, chunk by chunk.
};

struct response_buffer_tuple {
	long long deadline;	// 8B
	union {
		struct platform_event pe;
	} prv;				// 16B
	// 24B per a tuple
};

static const int RESPONSE_BUFFER_SIZE = 100;
struct response_buffer {
	int read_cursor;
	int write_cursor;
	pthread_mutex_t lock;
	struct response_buffer_tuple tuples[RESPONSE_BUFFER_SIZE];	// circular queue buffer
};

struct commands_flag {
	bool skip_waiting_in_replay;
	pthread_mutex_t lock;
};

static jmethodID method_didUpdateReplayingFields;
static jmethodID method_doLongSleepMS;

// Replayer's state
static const int RP_STATE_IDLE = 0;
static const int RP_STATE_READY_TO_FIRST_FETCHING = 1;
static const int RP_STATE_INITIAL_FETCHING = 2;
static const int RP_STATE_INITIAL_AND_FINAL_FETCHING = 3;
static const int RP_STATE_REPLAYING_AND_FETCHING = 4;
static const int RP_STATE_FINAL_FETCHING = 5;
static const int RP_STATE_ALL_FETCHED = 6;
static int gReplayerState = RP_STATE_IDLE;
static bool gIsThreadAlive = false;

// Global attributes
static long long gFinalSN = 0;	// it is valid only if state is 'RP_STATE_ALL_FETCHED'
static long long gRequiredSN = 1;

// ReplayBuffer & related attributes
static const int NUM_REPLAY_BUFFERS = 2;
static const long long INTERVAL_TO_DEADLINE_US = 60 * 1000 * 1000;
static int gReplayBuffers_Reading = 0;
static int gReplayBuffers_ReadCursor = 0;
static int gReplayBuffers_WriteCursor = 0;
static int gDefaultReplayBufferSize = 1;
static int gMaxSleepTimeMS = 0;
static struct replay_buffer gReplayBuffers[NUM_REPLAY_BUFFERS];

// ResponseBuffer & related attributes
static const int RESPONSE_BUFFER_VALIDMAP_SIZE = (RESPONSE_BUFFER_SIZE / 8) + 1;
static struct response_buffer gResponseBuffer;
static char gResponseBuffer_ValidMap[RESPONSE_BUFFER_VALIDMAP_SIZE];

static const long EVENT_TYPE_PLATFORM = 1;
static const long EVENT_TYPE_KERNEL_INPUT = 2;

// Commands flag
static struct commands_flag gCommandsFlag;

namespace android {
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

	// Replayer's states
	static inline bool is_doing_replay_routine() { 
		return (gReplayerState == RP_STATE_REPLAYING_AND_FETCHING
			|| gReplayerState == RP_STATE_FINAL_FETCHING
			|| gReplayerState == RP_STATE_ALL_FETCHED);
	}

	static inline bool is_initial_fetching() { 
		return (gReplayerState == RP_STATE_INITIAL_FETCHING
			|| gReplayerState == RP_STATE_INITIAL_AND_FINAL_FETCHING);
	}

	static inline bool is_final_fetching() { 
		return (gReplayerState == RP_STATE_INITIAL_AND_FINAL_FETCHING
			|| gReplayerState == RP_STATE_FINAL_FETCHING);
	}
	
	static inline bool is_fetching() {
		return (gReplayerState == RP_STATE_REPLAYING_AND_FETCHING
			|| is_initial_fetching()
			|| is_final_fetching());
	}

	static inline bool is_can_begin_to_fetching() { 
		return (gReplayerState == RP_STATE_READY_TO_FIRST_FETCHING
			|| gReplayerState == RP_STATE_REPLAYING_AND_FETCHING);
	}

	static inline bool is_ready_to_fetching_first_chunk() {
		return (gReplayerState == RP_STATE_READY_TO_FIRST_FETCHING);
	}

	static inline bool is_fetching_done() {
		return (gReplayerState == RP_STATE_ALL_FETCHED);
	}	

	static inline bool is_idle() {
		return (gReplayerState == RP_STATE_IDLE);
	}

	// Handling replay buffers
	static inline void inc_replay_buffers_read_cursor() {
		gReplayBuffers_Reading = gReplayBuffers_ReadCursor;
		gReplayBuffers_ReadCursor = (gReplayBuffers_ReadCursor + 1) % NUM_REPLAY_BUFFERS;
	}
	
	static inline void inc_replay_buffers_write_cursor() {
		gReplayBuffers_WriteCursor = (gReplayBuffers_WriteCursor + 1) % NUM_REPLAY_BUFFERS;
	}

	static inline struct replay_buffer* get_replay_buffer_for_read() {
		return &(gReplayBuffers[gReplayBuffers_ReadCursor]);
	}

	static inline struct replay_buffer* get_replay_buffer_for_write() {
		return &(gReplayBuffers[gReplayBuffers_WriteCursor]);
	}

	static inline struct replay_buffer* get_reading_replay_buffer() {
		return &(gReplayBuffers[gReplayBuffers_Reading]);
	}

	// Handling replay buffer
	static inline void init_replay_buffers() {
		for(int i = 0; i < NUM_REPLAY_BUFFERS; i++) {
			struct replay_buffer* rb = &(gReplayBuffers[i]);
			// Fill rb's attributes as meaningless values
			rb->sn = 0;
			rb->size = 0;
			rb->cursor = 0;

			pthread_mutex_init(&(rb->lock), NULL);

			// Fixed tuples is a memory area always-allocated during service is alive
			rb->fixed_tuples = (struct replay_buffer_tuple*)calloc(gDefaultReplayBufferSize, 
									sizeof(struct replay_buffer_tuple));

			// pointer to extra tuples is always NULL. its memory space will be allocated on demand.
			rb->extra_tuples = NULL;
		}
		gReplayBuffers_Reading = 0;
		gReplayBuffers_ReadCursor = 0; // initial replay buffers' read cursor is always 0
		gReplayBuffers_WriteCursor = 0; // initial replay buffers' write cursor is always 0
	}

	static inline void finalize_replay_buffers() {
		for(int i = 0; i < NUM_REPLAY_BUFFERS; i++) {
			struct replay_buffer* rb = &(gReplayBuffers[i]);
			if(rb->fixed_tuples != NULL) {
				free(rb->fixed_tuples);
				rb->fixed_tuples = NULL;
			}
			if(rb->extra_tuples != NULL) {
				free(rb->extra_tuples);
				rb->extra_tuples = NULL;
			}
		}
	}

	static inline long long get_replay_buffer_sn(struct replay_buffer* rb) {
		return rb->sn;
	}

	static inline int get_replay_buffer_size(struct replay_buffer* rb) {
		return rb->size;
	}
	
	static inline struct replay_buffer_tuple* get_replay_buffer_tuple(struct replay_buffer* rb) {
		struct replay_buffer_tuple* tuple;
		if(rb->cursor < gDefaultReplayBufferSize) {
			// in case of fixed tuples area:
			tuple = &(rb->fixed_tuples[rb->cursor]);
		}
		else {
			// in case of extra tuples area:
			tuple = &(rb->extra_tuples[rb->cursor - gDefaultReplayBufferSize]);
		}
		return tuple;
	}

	static inline int get_replay_buffer_cursor(struct replay_buffer* rb) {
		return rb->cursor;
	}

	static inline void reset_replay_buffer_cursor(struct replay_buffer* rb) {
		rb->cursor = 0;
	}

	static inline void inc_replay_buffer_cursor(struct replay_buffer* rb) {
		rb->cursor = rb->cursor + 1;
	}

	static inline bool is_replay_buffer_done(struct replay_buffer* rb) {
		return (rb->cursor >= rb->size);
	}

	static inline void lock_replay_buffer(struct replay_buffer* rb) {
		pthread_mutex_lock(&(rb->lock));
	}
	static inline void unlock_replay_buffer(struct replay_buffer* rb) {
		pthread_mutex_unlock(&(rb->lock));
	}

	// Handling response buffer
	static inline void init_response_buffer() {
		struct response_buffer *rpb = &(gResponseBuffer);
		rpb->read_cursor = 0;
		rpb->write_cursor = 0;
		memset(rpb->tuples, 0,
			sizeof(struct response_buffer_tuple) * RESPONSE_BUFFER_SIZE);
		pthread_mutex_init(&(rpb->lock), NULL);

		// Reset ResponseBuffer's valid map
		memset(gResponseBuffer_ValidMap, 0,
			RESPONSE_BUFFER_VALIDMAP_SIZE);
	}

	static inline void finalize_response_buffer() {
		// no finalization in response buffer
	}
	
	static inline struct response_buffer_tuple* get_response_buffer_tuple_for_read() {
		// It requires lock
		return &(gResponseBuffer.tuples[gResponseBuffer.read_cursor]);
	}

	static inline struct response_buffer_tuple* get_response_buffer_tuple_for_write() {
		// It requires lock
		return &(gResponseBuffer.tuples[gResponseBuffer.write_cursor]);
	}

	static inline void set_response_buffer_tuple_valid(int cursor, bool isValid) {
		// It requires lock
		int offset = cursor;
		int map_index = offset / 8;
		int map_offset = offset % 8;
		char mask = 0x01 << map_offset;
		if(isValid == true) {
			gResponseBuffer_ValidMap[map_index] = gResponseBuffer_ValidMap[map_index] | mask;
		} else {
			mask = ~mask;
			gResponseBuffer_ValidMap[map_index] = gResponseBuffer_ValidMap[map_index] & mask;
		}
	}

	static inline void set_response_buffer_tuple_valid_for_read(bool isValid) {
		// It requires lock
		set_response_buffer_tuple_valid(gResponseBuffer.read_cursor, isValid);
	}
	
	static inline void set_response_buffer_tuple_valid_for_write(bool isValid) {
		// It requires lock
		set_response_buffer_tuple_valid(gResponseBuffer.write_cursor, isValid);
	}

	static inline bool is_response_buffer_tuple_valid_for_read() {
		// It requires lock
		int offset = gResponseBuffer.read_cursor;
		int map_index = offset / 8;
		int map_offset = offset % 8;
		char mask = 0x01;
		if(((gResponseBuffer_ValidMap[map_index] >> map_offset) & mask) != 0) {
			return true;
		} else {
			return false;
		}
	}

	static inline bool is_response_buffer_tuple_valid_for_write(long long presentTime) {
		// It requires lock
		int offset = gResponseBuffer.write_cursor;
		int map_index = offset / 8;
		int map_offset = offset % 8;
		char mask = 0x01;
		if(((gResponseBuffer_ValidMap[map_index] >> map_offset) & mask) != 0) {
			struct response_buffer_tuple* tuple = get_response_buffer_tuple_for_write();
			if(tuple->deadline > presentTime)
				return true;
			else
				return false;
		} else {
			return false;
		}
	}

	static inline void inc_response_buffer_read_cursor() {
		// It requires lock
		gResponseBuffer.read_cursor = (gResponseBuffer.read_cursor + 1) % RESPONSE_BUFFER_SIZE;
	}

	static inline void inc_response_buffer_write_cursor() {
		// It requires lock
		gResponseBuffer.write_cursor = (gResponseBuffer.write_cursor + 1) % RESPONSE_BUFFER_SIZE;
	}

	static inline bool is_response_buffer_should_lock() {
		return (gResponseBuffer.read_cursor == gResponseBuffer.write_cursor);
	}

	static inline void lock_response_buffer() {
		pthread_mutex_lock(&(gResponseBuffer.lock));
	}
	static inline void unlock_response_buffer() {
		pthread_mutex_unlock(&(gResponseBuffer.lock));
	}

	// Handling sequence number
	static inline void init_required_sn(JNIEnv *env, jobject obj) {
		gRequiredSN = 1;
	}
	static inline void inc_required_sn(JNIEnv *env, jobject obj) {
		gRequiredSN++;
	}

	// Handling commands flag
	static inline void init_commands_flag() {
		gCommandsFlag.skip_waiting_in_replay = false;
		pthread_mutex_init(&(gCommandsFlag.lock), NULL);
	}
	static inline void set_skip_waiting_in_replay(bool skip_waiting_in_replay) {
		pthread_mutex_lock(&(gCommandsFlag.lock));
		gCommandsFlag.skip_waiting_in_replay = skip_waiting_in_replay;
		pthread_mutex_unlock(&(gCommandsFlag.lock));
	}
	static inline bool get_skip_waiting_in_replay() {
		bool ret;
		pthread_mutex_lock(&(gCommandsFlag.lock));
		ret = gCommandsFlag.skip_waiting_in_replay;
		pthread_mutex_unlock(&(gCommandsFlag.lock));
		return ret;
	}

	// Sleep functions
	static inline void short_sleep_nanoseconds(long long nsec) {
		// Short sleep: blocking sleep using CPU resources
		const int NSECS_TO_SECONDS = 1000 * 1000 * 1000;
		struct timespec timeout0;
		struct timespec timeout1;
		struct timespec* tmp;
		struct timespec* t0 = &timeout0;
		struct timespec* t1 = &timeout1;
	
		t0->tv_sec = (long)(nsec / NSECS_TO_SECONDS);
		t0->tv_nsec = (long)(nsec % NSECS_TO_SECONDS);
		
		while ((nanosleep(t0, t1) == (-1)) && (errno == EINTR)
			&& (gIsThreadAlive == true))
		{
			tmp = t0;
			t0 = t1;
			t1 = tmp;
		}
	}

	static inline void long_sleep_milliseconds(JNIEnv *env, jobject obj, int sleepMillis) {
		// Long sleep: it yields CPU resources until timer interrupt comes.
		// It uses Java thread sleep call, because of conflict between sleep system call and java sleep call

		// If maxSleepTimeMS is 0, sleep time is unlimited.
		if(gMaxSleepTimeMS != 0 && sleepMillis > gMaxSleepTimeMS)
			sleepMillis = gMaxSleepTimeMS;
		env->CallVoidMethod(obj, method_doLongSleepMS, (jint)sleepMillis);
	}

	static inline void sleep_nanoseconds(JNIEnv *env, jobject obj, long long nsec) {
		const int NSEC_TO_MILLISECONDS = 1000 * 1000;
		const int NSECS_TO_SECONDS = 1000 * 1000 * 1000;
		if(nsec < NSEC_TO_MILLISECONDS) {
			// under 1ms: short sleep
			short_sleep_nanoseconds(nsec);
		} else {
			// over 1ms: long sleep
			long_sleep_milliseconds(env, obj, nsec / NSEC_TO_MILLISECONDS);
		}
	}

	// Initialize & finalize
	static void initialize(int defaultReplayBufferSize, int maxSleepTimeMS) {
		// Initialize state
		gReplayerState = RP_STATE_IDLE;
		
		// Initialize global attributes
		gIsThreadAlive = false;
		gDefaultReplayBufferSize = defaultReplayBufferSize;
		gMaxSleepTimeMS = maxSleepTimeMS;
		init_commands_flag();

		// Initialize sequence numbers
		gRequiredSN = 1;

		// Initialize buffers
		init_replay_buffers();
		init_response_buffer();
	}

	static void finalize() {
		// Set global attributes		
		gIsThreadAlive = false;
		gRequiredSN = 1;

		// Finalize buffers
		finalize_replay_buffers();
		finalize_response_buffer();

		// Set state as idle
		gReplayerState = RP_STATE_IDLE;
	}

	// JNI handling
	static void checkAndClearExceptionFromCallback(JNIEnv* env, const char* methodName) {
		if (env->ExceptionCheck()) {
			ALOGE("An exception was thrown by callback '%s'.", methodName);
			LOGE_EX(env);
			env->ExceptionClear();
		}
	}

	static void android_recordroid_EventReplayer_class_init(JNIEnv* env, jclass clazz) {
		method_didUpdateReplayingFields = env->GetMethodID(clazz, "didUpdateReplayingFields", "(JJII)V");
		method_doLongSleepMS = env->GetMethodID(clazz, "doLongSleepMS", "(I)V");
	}

	static void android_recordroid_EventReplayer_init(JNIEnv* env, jobject obj,
			jint defaultReplayBufferSize, jint maxSleepTimeMS) {
		initialize(defaultReplayBufferSize, maxSleepTimeMS);
	}

	static void android_recordroid_EventReplayer_start_filling_buffer(JNIEnv* env, jobject obj,
			jboolean isNextExists, jint numEvents, jlong sn) {
		// This function runs only when 'receiving events'
		if(is_can_begin_to_fetching() == false)
			return;

		// Step 1. Get and lock the ReplayBuffer
		struct replay_buffer* rb = get_replay_buffer_for_write();
		lock_replay_buffer(rb);

		// Step 2. Set state if it is first chunk or final chunk
		// First & final chunk:
		if(is_ready_to_fetching_first_chunk()
			&& (isNextExists == JNI_FALSE))
			gReplayerState = RP_STATE_INITIAL_AND_FINAL_FETCHING;
		// First chunk:
		else if(is_ready_to_fetching_first_chunk())
			gReplayerState = RP_STATE_INITIAL_FETCHING;
		// Final chunk:
		else if(isNextExists == JNI_FALSE)
			gReplayerState = RP_STATE_FINAL_FETCHING;

		// Step 3. Initialize next ReplayBuffer
		rb->sn = sn;
		reset_replay_buffer_cursor(rb); // initial tuple cursor is always 0
		int old_size = rb->size;
		rb->size = numEvents;

		// Step 4. Re-allocate extra tuples area if required
		if(old_size != rb->size) {
			// De-allocate extra tuples
			if(rb->extra_tuples != NULL) {
				free(rb->extra_tuples);
				rb->extra_tuples = NULL;
			}

			// If extra tuples area is required, allocate it
			if(numEvents > gDefaultReplayBufferSize) {
				int num_extra_tuples = numEvents - gDefaultReplayBufferSize;
				rb->extra_tuples = (struct replay_buffer_tuple*)calloc(num_extra_tuples, 
										sizeof(struct replay_buffer_tuple));
			}
		}
	}

	static void android_recordroid_EventReplayer_set_is_alive(JNIEnv* env, jobject obj, 
		jboolean isAlive) {
		gIsThreadAlive = (isAlive == JNI_FALSE) ? false : true;
	}
	static jboolean android_recordroid_EventReplayer_get_is_alive(JNIEnv* env, jobject obj) {
		checkAndClearExceptionFromCallback(env, __FUNCTION__);
		return (gIsThreadAlive == false) ? JNI_FALSE : JNI_TRUE;
	}

	// Pop events from ReplayBuffer and replay them
	static int android_recordroid_EventReplayer_replay_routine(JNIEnv* env, jobject obj) {
		gIsThreadAlive = true;
		const long long DEFAULT_SLEEP_MS = 1;
		const long long US_TO_NANOSECS = 1000;
		int ret = 0;
		const int DEVICE_FDS_NUM = 100;
		int device_fds[DEVICE_FDS_NUM];
		int num_devices;
		struct replay_buffer* rb;

		#if LOG_OVERHEAD == 1
		long long initStartUS, initEndUS;
		initStartUS = get_uptime_microseconds();
		#endif

		// Kernel input event write cache
		bool ke_write_cache_forced_flush = false;
		const int SIZE_KE_WRITE_CACHE = 5;
		int num_ke_write_cache = 0;
		struct input_event ke_write_cache[SIZE_KE_WRITE_CACHE];
		memset(ke_write_cache, 0, sizeof(struct input_event) * SIZE_KE_WRITE_CACHE);

		// Open input devices
		{
			char device_path[] = "/dev/input";
			char device_file_path[PATH_MAX];
			char *device_file_name;
			
			DIR* dir;
			struct dirent* de;

			// Allocate memory area for file descriptors
			num_devices = 0;
			dir = opendir(device_path);
			if(dir == NULL) {
				ALOGE("cannot access to %s", device_path);
				goto quit;
			}
			strcpy(device_file_path, device_path);
			device_file_name = device_file_path + strlen(device_file_path);
			*device_file_name++ = '/';
			while((de = readdir(dir))) {
				if(de->d_name[0] == '.' &&
						(de->d_name[1] == '\0' ||
						 (de->d_name[1] == '.' && de->d_name[2] == '\0')))
					continue;
				if((strlen(de->d_name) < 6) || 
					(strncmp(de->d_name, "event", strlen("event") != 0)))
					continue;
				int num = (int)(de->d_name[strlen(de->d_name) - 1] - '0');
				num_devices = (num_devices < num) ? num : num_devices;
			}
			closedir(dir);
			memset(device_fds, 0, sizeof(int) * DEVICE_FDS_NUM);

			// Open device files
			dir = opendir(device_path);
			if(dir == NULL) {
				ALOGE("cannot access to %s", device_path);
				goto quit;
			}
			strcpy(device_file_path, device_path);
			device_file_name = device_file_path + strlen(device_file_path);
			*device_file_name++ = '/';
			while((de = readdir(dir))) {
				if(de->d_name[0] == '.' &&
						(de->d_name[1] == '\0' ||
						 (de->d_name[1] == '.' && de->d_name[2] == '\0')))
					continue;
				if((strlen(de->d_name) < 6) || 
					(strncmp(de->d_name, "event", strlen("event") != 0)))
					continue;
				strcpy(device_file_name, de->d_name);
				int num = (int)(de->d_name[strlen(de->d_name) - 1] - '0');
				int version;
				device_fds[num] = open(device_file_path, O_RDWR);
				if(device_fds[num] <= 0) {
					ALOGE("Device file cannot be opened: %s", device_file_path);
				}
				if(ioctl(device_fds[num], EVIOCGVERSION, &version)) {
					ALOGE("Device file is out of version: %s", device_file_path);
				}
			}
			closedir(dir);
		}

		// Ready to fetch first chunk
		gReplayerState = RP_STATE_READY_TO_FIRST_FETCHING;
		init_required_sn(env, obj);

		#if LOG_OVERHEAD == 1
		initEndUS = get_uptime_microseconds();
		ALOGE("Initialization done %lld", initEndUS - initStartUS);
		#endif
		
		// Wait until initial fetching of replay buffer is done
		while(is_ready_to_fetching_first_chunk() || is_initial_fetching())
			sleep(1);
		// Get the first replay buffer for read
		rb = get_replay_buffer_for_read();
		inc_replay_buffers_read_cursor();
		lock_replay_buffer(rb);

		// replay routine LOOP start
		while(gIsThreadAlive == true) {
			#if LOG_OVERHEAD == 1
			long long startUS, handlingStartUS, sleepingStartUS, sleepingEndUS, endUS;
			handlingStartUS = sleepingStartUS = endUS = 0;
			startUS = get_uptime_microseconds();
			#endif
			
			// If current buffer is done, go to next buffer or quit
			if(is_replay_buffer_done(rb) == true) {
				unlock_replay_buffer(rb);
				if(is_fetching_done() && gFinalSN == rb->sn) {
					// If previous buffer was final: go out
					goto close_device_files;
				} else {
					// In case of others: get next buffer
					rb = get_replay_buffer_for_read();
					inc_replay_buffers_read_cursor();
					lock_replay_buffer(rb);
					reset_replay_buffer_cursor(rb);
				}
			}
			// Pop a tuple from replay buffer
			struct replay_buffer_tuple* rb_tuple;
			rb_tuple = get_replay_buffer_tuple(rb);
			if(gIsThreadAlive == false)
				goto force_unlock;
			
			// replay buffer TUPLE handling routine start
			if(rb_tuple->eventType == EVENT_TYPE_KERNEL_INPUT) {
				// Kernel input event: move to write cache and flush the cache on condition

				#if LOG_OVERHEAD == 1
				handlingStartUS = get_uptime_microseconds();
				#endif

				// Move to write cache
				// if timestamp is 0, put event to pe_write_cache
				int index = num_ke_write_cache;
				ke_write_cache[index].type = rb_tuple->prv.ke.typeVal;
				ke_write_cache[index].code = rb_tuple->prv.ke.codeVal;
				ke_write_cache[index].value = rb_tuple->prv.ke.value;
				num_ke_write_cache++;
				
				if(rb_tuple->timestamp != 0) {
					// Should flush right after sleep
					ke_write_cache_forced_flush = true;

					#if LOG_OVERHEAD == 1
					sleepingStartUS = get_uptime_microseconds();
					#endif
					
					// Sleep
					sleep_nanoseconds(env, obj, rb_tuple->timestamp * US_TO_NANOSECS);

					#if LOG_OVERHEAD == 1
					sleepingEndUS = get_uptime_microseconds();
					#endif
				}
				if(gIsThreadAlive == false) {
					goto force_unlock;
				}

				// Flush kernel input event write cache if it is full or forced-flush flag is set
				if(num_ke_write_cache >= SIZE_KE_WRITE_CACHE
					|| ke_write_cache_forced_flush == true) {
					// Flush kernel input event
					int events_to_write_size = sizeof(struct input_event) * num_ke_write_cache;
					int retValue = write(device_fds[rb_tuple->prv.ke.deviceNum],
						ke_write_cache, events_to_write_size);
					if(retValue < events_to_write_size) {
						ALOGE("Write event failed... (%dB should be written, but %dB is written.",
							events_to_write_size, retValue);
						goto force_unlock;
					}

					// Reset write cache
					ke_write_cache_forced_flush = false;
					num_ke_write_cache = 0;
				}
			} 
			else if(rb_tuple->eventType == EVENT_TYPE_PLATFORM) {
				long long slp = rb_tuple->timestamp * US_TO_NANOSECS;
				// Platform event: wait until the event comes
				// Sleep
				#if LOG_OVERHEAD == 1
				handlingStartUS = sleepingStartUS = get_uptime_microseconds();
				#endif
				sleep_nanoseconds(env, obj, rb_tuple->timestamp * US_TO_NANOSECS);
				#if LOG_OVERHEAD == 1
				sleepingEndUS = get_uptime_microseconds();
				#endif
				
				if(gIsThreadAlive == false)
					goto force_unlock;
				bool is_found = false;
				int sleep_ms = DEFAULT_SLEEP_MS;
				bool tupleLocked = false;
				while((is_found == false) && (gIsThreadAlive == true)
					&& (get_skip_waiting_in_replay() == false)) {
					// Scan whole response event
					for(int i = 0; (i < RESPONSE_BUFFER_SIZE) && (is_found == false); i++) {
						// Lock the buffer if read cursor and write cursor are overlapped
						if(is_response_buffer_should_lock() == true) {
							tupleLocked = true;
							lock_response_buffer();
						}

						// Check the tuple
						struct response_buffer_tuple* rpb_tuple;
						rpb_tuple = get_response_buffer_tuple_for_read();
						if((is_response_buffer_tuple_valid_for_read() == true)
							&& (rb_tuple->prv.pe.peType == rpb_tuple->prv.pe.peType)
							&& (rb_tuple->prv.pe.priv == rpb_tuple->prv.pe.priv)
							&& (rb_tuple->prv.pe.second_priv == rpb_tuple->prv.pe.second_priv)) {
							set_response_buffer_tuple_valid_for_read(false);
							is_found = true;
						}

						if(tupleLocked == true) {
							unlock_response_buffer();
							tupleLocked = false;
						}
						
						inc_response_buffer_read_cursor();

						//ALOGE("waiting for platform event: %d/%d", i, RESPONSE_BUFFER_SIZE);
					}
					
					if(is_found == false) {
						long_sleep_milliseconds(env, obj, sleep_ms);
						if(sleep_ms < 10) {
							// under 10ms: double sleep time
							sleep_ms = sleep_ms * 2;
						} else {
							// maximum limit = 10ms
							sleep_ms = 10;
						}
					}
				}
				set_skip_waiting_in_replay(false);
			} // The end of replay buffer TUPLE handling routine

			// increase replay buffer's cursor
			inc_replay_buffer_cursor(rb);
			
			#if LOG_OVERHEAD == 1
			endUS = get_uptime_microseconds();
			#endif

			// Log replaying overhead
			#if LOG_OVERHEAD == 1
			long long interval = (endUS - startUS);
			long long totalHandlingInterval = (endUS - handlingStartUS);
			long long sleepingInterval = (sleepingEndUS- sleepingStartUS);
			long long pureHandlingInterval = totalHandlingInterval - sleepingInterval;
			long long headInterval = (interval - totalHandlingInterval);
			// Total = Head + Pure sleep + Tail(includes I/O)
			ALOGE("TUPLE %d.%d\t HEAD:\t%lld\t PURE:\t%lld\t SLP:\t%lld\t ORG-SLP:\t%lld",
				(gReplayBuffers_ReadCursor + 1) % 2, rb->cursor,
				headInterval, pureHandlingInterval, sleepingInterval, rb_tuple->timestamp);
			#endif
		} // The end of replay routine LOOP
	force_unlock:
		unlock_replay_buffer(rb);
	close_device_files:
		// Close device files
		for(int i=0; i<num_devices; i++) {
			if(device_fds[i] != 0) close(device_fds[i]);
		}
	quit:
		gIsThreadAlive = false;
		finalize();
		return ret;
	}

	static inline void check_replay_buffer_done(JNIEnv* env, jobject obj, struct replay_buffer* rb) {
		// When writing to the replay buffer is done:
		if(is_replay_buffer_done(rb) == true) {
			if(is_final_fetching() == true) {
				// Final / Initial & Final chunk:
				gReplayerState = RP_STATE_ALL_FETCHED;
				gFinalSN = rb->sn;
			} else if(is_initial_fetching()) {
				// First chunk:
				gReplayerState = RP_STATE_REPLAYING_AND_FETCHING;
			}

			reset_replay_buffer_cursor(rb);
			inc_replay_buffers_write_cursor();

			// If it is fetching replay buffers yet, increase required sequence number
			if(is_fetching() == true)
				inc_required_sn(env, obj);
			
			// Unlock the replay bfffer
			unlock_replay_buffer(rb);
		}
	}

	// KernelInputEvent from host -> ReplayBuffer
	static void android_recordroid_EventReplayer_fetch_kernel_input_event(JNIEnv* env, jobject obj,
			jobject event) { // RecordroidKernelInputEvent event
		if(is_fetching() == false || gIsThreadAlive == false)
			return;

		// Get field values
		jclass class_RecordroidKernelInputEvent = env->GetObjectClass(event);
		jfieldID id_timestampUS = env->GetFieldID(class_RecordroidKernelInputEvent,
				"timestampUS", "J");
		jfieldID id_deviceNum = env->GetFieldID(class_RecordroidKernelInputEvent,
				"deviceNum", "I");
		jfieldID id_typeVal = env->GetFieldID(class_RecordroidKernelInputEvent,
				"typeVal", "I");
		jfieldID id_codeVal = env->GetFieldID(class_RecordroidKernelInputEvent,
				"codeVal", "I");
		jfieldID id_value = env->GetFieldID(class_RecordroidKernelInputEvent,
				"value", "I");
		
		jlong timestampUS = env->GetLongField(event, id_timestampUS);
		jint deviceNum = env->GetIntField(event, id_deviceNum);
		jint typeVal = env->GetIntField(event, id_typeVal);
		jint codeVal = env->GetIntField(event, id_codeVal);
		jint value = env->GetIntField(event, id_value);
		
		// Initialize tuple's contents
		struct replay_buffer* rb = get_replay_buffer_for_write();
		struct replay_buffer_tuple* tuple = get_replay_buffer_tuple(rb);
		
		tuple->eventType = EVENT_TYPE_KERNEL_INPUT;
		tuple->timestamp = (long long)timestampUS;
		tuple->prv.ke.deviceNum = (long)deviceNum;
		tuple->prv.ke.typeVal = (long)typeVal;
		tuple->prv.ke.codeVal = (long)codeVal;
		tuple->prv.ke.value = (long)value;
		inc_replay_buffer_cursor(rb);

		// Handling to finalize fetching the replay buffer 
		check_replay_buffer_done(env, obj, rb);
	}

	// PlatformEvent from host -> ReplayBuffer
	static void android_recordroid_EventReplayer_fetch_platform_event(JNIEnv* env, jobject obj,
			jobject event) { // RecordroidPlatformEvent event
		if(is_fetching() == false || gIsThreadAlive == false)
			return;

		// Get field values
		jclass class_RecordroidPlatformEvent = env->GetObjectClass(event);
		jfieldID id_timestampUS = env->GetFieldID(class_RecordroidPlatformEvent,
				"timestampUS", "J");
		jfieldID id_platformEventType = env->GetFieldID(class_RecordroidPlatformEvent,
				"platformEventType", "I");
		jfieldID id_responseTimeUS = env->GetFieldID(class_RecordroidPlatformEvent,
				"responseTimeUS", "I");
		jfieldID id_priv = env->GetFieldID(class_RecordroidPlatformEvent,
				"priv", "I");
		jfieldID id_secondPriv = env->GetFieldID(class_RecordroidPlatformEvent,
				"secondPriv", "I");
		
		jlong timestampUS = env->GetLongField(event, id_timestampUS);
		jint platformEventType = env->GetIntField(event, id_platformEventType);
		jint responseTimeUS = env->GetIntField(event, id_responseTimeUS);
		jint priv = env->GetIntField(event, id_priv);
		jint secondPriv = env->GetIntField(event, id_secondPriv);

		// Initialize tuple's contents
		struct replay_buffer* rb = get_replay_buffer_for_write();
		struct replay_buffer_tuple* tuple = get_replay_buffer_tuple(rb);
		
		tuple->eventType = EVENT_TYPE_PLATFORM;
		tuple->timestamp = (long long)timestampUS;
		tuple->prv.pe.peType = (long)platformEventType;
		tuple->prv.pe.response_time_us = (long)responseTimeUS;
		tuple->prv.pe.priv = (long)priv;
		tuple->prv.pe.second_priv = (long)secondPriv;

		// increase replay buffer's cursor
		inc_replay_buffer_cursor(rb);

		// Handling to finalize fetching the replay buffer 
		check_replay_buffer_done(env, obj, rb);
	}

	// PlatformEvent from PlatformEventPoller -> ResponseBuffer
	static void android_recordroid_EventReplayer_on_poll_platform_event(JNIEnv* env, jobject obj,
			jobject event) { // RecordroidPlatformEvent event
		if(is_doing_replay_routine() == false || gIsThreadAlive == false)
			return;
		// find empty(invalid) response buffer tuple & lock the response buffer
		int steps = 0;
		long long stepTimestamp = 0;
		bool tupleLocked = false;
		while(true) {
			// On step 0, set timestamp
			if(steps == 0)
				stepTimestamp = get_uptime_microseconds();

			// Lock the buffer if read cursor and write cursor are overlapped
			if(is_response_buffer_should_lock() == true) {
				tupleLocked = true;
				lock_response_buffer();
			}

			if(is_response_buffer_tuple_valid_for_write(stepTimestamp) == true)
				inc_response_buffer_write_cursor();
			else
				break;
			
			if(tupleLocked == true)
				unlock_response_buffer();

			//ALOGE("onPollPlatformEvent: %d/%d", steps, RESPONSE_BUFFER_SIZE);
			steps++;
			if(steps >= RESPONSE_BUFFER_SIZE)
				steps = 0;
		}
		
		struct response_buffer_tuple* tuple = get_response_buffer_tuple_for_write();
		// Get field values
		jclass class_RecordroidPlatformEvent = env->GetObjectClass(event);
		jfieldID id_timestampUS = env->GetFieldID(class_RecordroidPlatformEvent,
				"timestampUS", "J");
		jfieldID id_platformEventType = env->GetFieldID(class_RecordroidPlatformEvent,
				"platformEventType", "I");
		jfieldID id_responseTimeUS = env->GetFieldID(class_RecordroidPlatformEvent,
				"responseTimeUS", "I");
		jfieldID id_priv = env->GetFieldID(class_RecordroidPlatformEvent,
				"priv", "I");
		jfieldID id_secondPriv = env->GetFieldID(class_RecordroidPlatformEvent,
				"secondPriv", "I");

		jlong timestampUS = env->GetIntField(event, id_timestampUS);
		jint platformEventType = env->GetIntField(event, id_platformEventType);
		jint responseTimeUS = env->GetIntField(event, id_responseTimeUS);
		jint priv = env->GetIntField(event, id_priv);
		jint secondPriv = env->GetIntField(event, id_secondPriv);

		// Initialize tuple's contents
		tuple->deadline = (long long)timestampUS + INTERVAL_TO_DEADLINE_US;
		tuple->prv.pe.peType = (long)platformEventType;
		tuple->prv.pe.response_time_us = (long)responseTimeUS;
		tuple->prv.pe.priv = (long)priv;
		tuple->prv.pe.second_priv = (long)secondPriv;

		// Set valid array
		set_response_buffer_tuple_valid_for_write(true);

		// unlock the response buffer
		if(tupleLocked == true) {
			unlock_response_buffer();
			tupleLocked = false;
		}

		// increase response buffer's write cursor
		inc_response_buffer_write_cursor();
	}

	static void android_recordroid_EventReplayer_skip_waiting_in_replay(JNIEnv* env, jobject obj) {
		if(is_doing_replay_routine() == true) {
			set_skip_waiting_in_replay(true);
		}
	}

	static void android_recordroid_EventReplayer_update_replaying_fields(JNIEnv* env, jobject obj) {
		struct replay_buffer* rb = get_reading_replay_buffer();
		long long requiredSN, presentSN;
		int presentReplayBufferIndex, presentReplayBufferSize;
		
		requiredSN = gRequiredSN;
		if(is_doing_replay_routine()) {
			presentSN = get_replay_buffer_sn(rb);
			presentReplayBufferIndex = get_replay_buffer_cursor(rb);
			presentReplayBufferSize = get_replay_buffer_size(rb);
		} else {
			presentSN = 0;
			presentReplayBufferIndex = 0;
			presentReplayBufferSize = 0;
		}
		
		env->CallVoidMethod(obj, method_didUpdateReplayingFields, 
			(jlong)gRequiredSN, (jlong)presentSN,
			(jint)presentReplayBufferIndex, (jint)presentReplayBufferSize);
	}


	static JNINativeMethod sMethods[] = {
		{"native_class_init", "()V", (void *)android_recordroid_EventReplayer_class_init},
		{"native_init", "(II)V", (void *)android_recordroid_EventReplayer_init},
		{"native_start_filling_buffer", "(ZIJ)V", (void *)android_recordroid_EventReplayer_start_filling_buffer},
		{"native_set_is_alive", "(Z)V", (void *)android_recordroid_EventReplayer_set_is_alive},
		{"native_get_is_alive", "()Z", (void *)android_recordroid_EventReplayer_get_is_alive},
		{"native_replay_routine", "()V", (void *)android_recordroid_EventReplayer_replay_routine},
		{"native_fetch_kernel_input_event", "(Lcom/android/server/recordroid/RecordroidKernelInputEvent;)V", (void *)android_recordroid_EventReplayer_fetch_kernel_input_event},
		{"native_fetch_platform_event", "(Lcom/android/server/recordroid/RecordroidPlatformEvent;)V", (void *)android_recordroid_EventReplayer_fetch_platform_event},
		{"native_on_poll_platform_event", "(Lcom/android/server/recordroid/RecordroidPlatformEvent;)V", (void *)android_recordroid_EventReplayer_on_poll_platform_event},
		{"native_skip_waiting_in_replay", "()V", (void *)android_recordroid_EventReplayer_skip_waiting_in_replay},
		{"native_update_replaying_fields", "()V", (void *)android_recordroid_EventReplayer_update_replaying_fields},
	};

	int register_android_server_recordroid_EventReplayer(JNIEnv* env)
	{
		return jniRegisterNativeMethods(env, "com/android/server/recordroid/EventReplayer", sMethods, NELEM(sMethods));
	}
}
