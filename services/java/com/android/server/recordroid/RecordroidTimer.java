package com.android.server.recordroid;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Hashtable;
import java.util.LinkedList;
import java.util.Set;

import android.content.Context;
import android.os.SystemClock;
import android.util.EventLog;
import android.util.Log;

public class RecordroidTimer {
	private static final String TAG = "RecordroidTimer";
	
	static { native_class_init(); }

	public RecordroidTimer() {
		native_init();
	}

	// Initialize native context
	private static native void native_class_init();
	private native void native_init();

	private static native Timestamp native_get_uptime();

	public static Timestamp getUptimeNS() { 
		/*final int NSECS_TO_SECS = 1000*1000*1000;
		final int MSECS_TO_NSECS = 1000*1000;
		//long timestampNS = System.currentTimeMills() * MSECS_TO_NSECS;
		long tv_sec = System.currentTimeMills() / 1000;
		long tv_nsec = (System.currentTimeMills() % 1000) * 1000 * 1000;
		Timestamp ret = new Timestamp(tv_sec, tv_nsec);*/
		Timestamp ret = native_get_uptime();
		return ret;
	}
}

class Timestamp {
	public Timestamp(long sec, long nsec) {
		this.sec = sec;
		this.nsec = nsec;
	}
	public long sec;
	public long nsec;

	public long toUS() {
		final int SECS_TO_USECS = 1000*1000;
		final int NSECS_TO_USECS = 1000;
		return this.sec * SECS_TO_USECS + (this.nsec / NSECS_TO_USECS);
	}
}
