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

public class KernelInputEventPoller {
	private static final String TAG = "KernelInputEventPoller";

	private PollingWorker mPollingWorker;
	private ChunkingWorker mChunkingWorker;
	private ArrayList<RecordroidEventListener> mEventListeners;
	
	static { native_class_init(); }
	
	public KernelInputEventPoller() {
		native_init();
		this.mEventListeners = new ArrayList<RecordroidEventListener>();
	}

	public void startThread() {
		// Initialize zero time as start time
				
		this.mPollingWorker = new PollingWorker();
		this.mPollingWorker.start();

		this.mChunkingWorker = new ChunkingWorker();
		this.mChunkingWorker.start();
	}

	public void killThread() {	
		//Main thread should wait until poller thread is completely killed
		try {
			this.mPollingWorker.kill();
			this.mPollingWorker.join();
			this.mChunkingWorker.kill();
			this.mChunkingWorker.join();
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

	// Initialize native context
	private static native void native_class_init();
	private native void native_init();
	//private native void native_init_zero_time(long tv_sec, long tv_usec);

	// Poll & chunk kernel input events
	private native int native_poll();
	private native int native_chunk(boolean isUrgentChunk);

	private native void native_set_is_polling_worker_alive(boolean isPollingWorkerAlive);
	private native boolean native_is_polling_worker_alive();

	// Called by native function 'native_chunk()'
	// Make KernelInputEvent from several numbers and send to listeners
	private void completePoll(long ts_sec, long ts_usec,
		int deviceNum, int typeVal, int codeVal, int value) {
		RecordroidKernelInputEvent newEvent
			= RecordroidKernelInputEvent.make(ts_sec, ts_usec,
				deviceNum, typeVal, codeVal, value);
		for(RecordroidEventListener listener: this.mEventListeners) {
			listener.onPollEvent(newEvent);
		}
	}
	
	class ChunkingWorker extends Thread {
		private static final String THREAD_NAME = "1KernelInputEventChunkingThread";
		private static final int SLEEP_MILLISECONDS = 1000;
		private boolean mIsRunning = false;

		public ChunkingWorker() {
			super(THREAD_NAME);
		}

		public void run() {
			this.mIsRunning = true;
			
			while(this.mIsRunning == true) {
				try {
					int res = native_chunk(false);
					Thread.sleep(SLEEP_MILLISECONDS);
				} catch(InterruptedException e) {
					Log.e(TAG, "InterruptedException");
				}
			}
			native_chunk(true);
		}

		public void kill() {
			this.mIsRunning = false;
		}
	}

	class PollingWorker extends Thread {
		private static final String THREAD_NAME = "2KernelInputEventPollingThread";
		public PollingWorker() {
			super(THREAD_NAME);
		}

		public boolean isRunning() {
			return native_is_polling_worker_alive();
		}

		public void run() {
			native_set_is_polling_worker_alive(true);
			
			while(native_is_polling_worker_alive() == true) {
				int res = native_poll();
				// No sleep
			}
		}

		public void kill() {
			native_set_is_polling_worker_alive(false);
		}
	}
}
