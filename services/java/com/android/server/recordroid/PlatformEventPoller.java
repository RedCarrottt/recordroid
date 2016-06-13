package com.android.server.recordroid;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Collection;
import java.util.LinkedList;

import android.content.Context;
import android.util.EventLog;
import android.util.Log;

public class PlatformEventPoller {
	private static final String TAG = "PlatformEventPoller";

	private long mLastPolledTime = 0L;
	private Worker mWorker;
	private ArrayList<RecordroidEventListener> mEventListeners;
	private long mZeroTimestampUS;

	public PlatformEventPoller() {
		// Initialize EventListeners' array
		this.mEventListeners = new ArrayList<RecordroidEventListener>();
	}

	public void startThread() {
		// Initialize zero time as start time
		this.mZeroTimestampUS = RecordroidTimer.getUptimeNS().toUS();

		RecordroidPlatformEventQueue.get().enable();
		
		this.mWorker = new Worker();
		this.mWorker.setPriority(10);
		this.mWorker.start();
	}

	public void killThread() {
		this.mWorker.kill();

		RecordroidPlatformEventQueue.get().disable();
		
		// FIXED: Main thread should wait until poller thread is completely killed
		try {
			this.mWorker.join();
		} catch (InterruptedException e) {
			e.printStackTrace();
		}
	}

	public void addListener(RecordroidEventListener listener) {
		this.mEventListeners.add(listener);
	}

	public void removeListener(RecordroidEventListener listener) {
		this.mEventListeners.remove(listener);
	}

	private void poll(boolean isUrgentPop) {
		//Log.e(TAG, "poll() start");
		// Step 1. Poll Recordroid platform events from Android platform
		RecordroidPlatformEventQueue eventQueue = RecordroidPlatformEventQueue.get();
		ArrayList<RecordroidPlatformEvent> polledEvents = eventQueue.popAll(isUrgentPop);

		// Step 2. If some events are polled, emit 'poll event'
		if(polledEvents != null && polledEvents.isEmpty() == false) {
			for(RecordroidEventListener listener: this.mEventListeners) {
				for(RecordroidPlatformEvent event: polledEvents) {
					// prune events before zero time
					if(event.timestampUS > this.mZeroTimestampUS) {
						listener.onPollEvent(event);
					}
				}
			}
		}
		//Log.e(TAG, "poll() end");
	}

	class Worker extends Thread {
		private static final String THREAD_NAME = "PlatformEventPollerThread";
		private static final int SLEEP_MILLISECONDS = 1000;
		private boolean mIsRunning = false;

		public Worker() {
			super(THREAD_NAME);
		}

		public boolean isRunning() {
			return this.mIsRunning;
		}

		public void run() {
			this.mIsRunning = true;
			while(this.mIsRunning == true) {
				//Log.e(TAG, "poller run() 1");
				try {
					poll(false);
					//Log.e(TAG, "poller run() 2");
					Thread.sleep(SLEEP_MILLISECONDS);
					//Log.e(TAG, "poller run() 3");
				} catch(InterruptedException e) {
					Log.e(TAG, "InterruptedException");
				}
				//Log.e(TAG, "poller run() 4");
			}
			//Log.e(TAG, "poller run() 5");
			// On finalizing phase, it should do one more 'urgent pop'.
			poll(true);
			//Log.i(TAG, "Platform event poller thread is finished");
		}
		public void kill() {
			this.mIsRunning = false;
		}
	}
}
