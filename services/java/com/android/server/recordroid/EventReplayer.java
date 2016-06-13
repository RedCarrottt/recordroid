package com.android.server.recordroid;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Hashtable;
import java.util.LinkedList;
import java.util.Set;

import android.content.Context;
import android.util.EventLog;
import android.util.Log;

public class EventReplayer implements RecordroidEventListener {
	private static final String TAG = "EventReplayer";
	private Worker mWorker;
	private ArrayList<EventReplayerStateListener> mListeners;
	private Boolean mIsInLongSleep = false;

	static { native_class_init(); }

	public EventReplayer() {
		this.mListeners = new ArrayList<EventReplayerStateListener>();
	}

	public void startThread(int defaultReplayBufferSize, int maxSleepTimeMS) {
		this.native_init(defaultReplayBufferSize, maxSleepTimeMS);
		this.mIsInLongSleep = false;
		
		this.mWorker = new Worker();
		this.mWorker.start();
	}

	public void killThread() {
		this.mWorker.kill();
	}

	public void addListener(EventReplayerStateListener listener) {
		this.mListeners.add(listener);
	}

	public void removeListener(EventReplayerStateListener listener) {
		this.mListeners.remove(listener);
	}

	// Initialize & Finalize native context
	private static native void native_class_init();
	private native void native_init(int replayBufferSize, int maxSleepTimeMS);

	private native void native_start_filling_buffer(boolean isNextExists, int numEvents, long sn);
	private native void native_set_is_alive(boolean isAlive);
	private native boolean native_get_is_alive();
	private native void native_replay_routine();

	// Fetch events from controller
	private native void native_fetch_kernel_input_event(RecordroidKernelInputEvent event);
	private native void native_fetch_platform_event(RecordroidPlatformEvent event);

	// Fetch platform events from PlatformEventPoller
	private native void native_on_poll_platform_event(RecordroidPlatformEvent event);

	// Handlers for commands from host
	private native void native_skip_waiting_in_replay();
	private native void native_update_replaying_fields();

	public void skipWaitingInReplay() {
		synchronized(this.mIsInLongSleep) {
			if(this.mIsInLongSleep) {
				this.wakeUpFromSleep();
			} else {
				this.native_skip_waiting_in_replay();
			}
		}
	}

	public void updateReplayingFields() {
		this.native_update_replaying_fields();
	}

	private void didUpdateReplayingFields(long requiredSN, 
		long runningSN, int presentReplayBufferIndex,
		int presentReplayBufferSize) {
		for(EventReplayerStateListener l: this.mListeners) {
			l.didUpdateReplayingFields(requiredSN, runningSN,
				presentReplayBufferIndex, presentReplayBufferSize);
		}
	}

	private void doLongSleepMS(int sleepMillis) {
		synchronized(this.mIsInLongSleep) {
			this.mIsInLongSleep = true;
		}
		try {
			Thread.sleep(sleepMillis);
		} catch(InterruptedException e) {
			// Interrupted
			Log.e(TAG, "Skipped to sleep!");
		}
		synchronized(this.mIsInLongSleep) {
			this.mIsInLongSleep = false;
		}
	}

	private void wakeUpFromSleep() {
		this.mWorker.interrupt();
	}

	public void startFillingBuffer(boolean isNextExists, int numEvents, long sn) {
		this.native_start_filling_buffer(isNextExists, numEvents, sn);
	}

	// Listen event trace from target via USB interface and fetch on EventQueue
	public void fetchKernelInputEvent(RecordroidKernelInputEvent event) {
		this.native_fetch_kernel_input_event(event);
	}

	public void fetchPlatformEvent(RecordroidPlatformEvent event) {
		this.native_fetch_platform_event(event);
	}

	// Implements RecordroidEventListener
	// Listen platform events from poller and utilize it to condition of replaying next input events
	public void onPollEvent(RecordroidEvent event) {
		Log.d(TAG, "onPollEvent()");
		if(event instanceof RecordroidPlatformEvent)
			native_on_poll_platform_event((RecordroidPlatformEvent)event);
	}
	
	class Worker extends Thread {
		private static final String THREAD_NAME = "EventReplayerThread";

		public Worker() {
			super(THREAD_NAME);
		}
		public void run() {
			native_replay_routine();
			Log.i(TAG, "Replayer thread routine done");
			for(EventReplayerStateListener l: mListeners) {
				l.didFinishReplaying();
			}
			Log.i(TAG, "Replayer thread finalization done");
		}
		public void kill() {
			native_set_is_alive(false);
		}
	}
}

interface EventReplayerStateListener {
	public void didUpdateReplayingFields(long requiredSN, 
		long runningSN, int presentReplayBufferIndex,
		int presentReplayBufferSize);
	public void didFinishReplaying();
}
