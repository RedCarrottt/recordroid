package com.android.server.recordroid;

import java.util.ArrayList;
import java.util.concurrent.locks.Lock;
import java.util.concurrent.locks.ReentrantLock;
import java.util.Iterator;

import android.os.SystemClock;
import android.view.MotionEvent;
import android.view.KeyEvent;
import android.util.Log;

public class RecordroidPlatformEventQueue {
	private static final String TAG = "RecordroidPlatformEventQueue";

	private static RecordroidPlatformEventQueue singleton;

	private boolean mEnabled = false;

	private ViewInputEventQueue mViewInputEventQueue;
	private WebPageLoadEventQueue mWebPageLoadEventQueue;
	private ActivityLaunchEventQueue mActivityLaunchEventQueue;
	private ActivityPauseEventQueue mActivityPauseEventQueue;
	private ViewShortClickEventQueue mViewShortClickEventQueue;
	private ViewLongClickEventQueue mViewLongClickEventQueue;

	private RecordroidPlatformEventQueue() {
		this.mViewInputEventQueue = new ViewInputEventQueue();
		this.mWebPageLoadEventQueue = new WebPageLoadEventQueue();
		this.mActivityLaunchEventQueue = new ActivityLaunchEventQueue();
		this.mActivityPauseEventQueue = new ActivityPauseEventQueue();
		this.mViewShortClickEventQueue = new ViewShortClickEventQueue();
		this.mViewLongClickEventQueue = new ViewLongClickEventQueue();
	}

	public static RecordroidPlatformEventQueue get() {
		if(singleton == null) {
			singleton = new RecordroidPlatformEventQueue();
		}
		return singleton;
	}

	public void enable() {
		this.mViewInputEventQueue.popAll(true);
		this.mWebPageLoadEventQueue.popAll(true);
		this.mActivityLaunchEventQueue.popAll(true);
		this.mEnabled = true;
	}

	public void disable() {
		this.mEnabled = false;
	}

	public boolean isEnabled() {
		return this.mEnabled;
	}

	public ArrayList<RecordroidPlatformEvent> popAll(boolean isUrgentPop) {
		// isUrgentPop:
		//   If PlatformEventPollerThread should be killed right now,
		//   it should pop events from queues as many as possible
		//   by easing the condition of 'completed event'.
		//   In this case, caller should call this function with 'isUrgentPop' flag.
		ArrayList<RecordroidPlatformEvent> events = null;
		ArrayList<RecordroidPlatformEvent> popped = null;
		popped = this.mViewInputEventQueue.popAll(isUrgentPop);
		if(popped != null) {
			if(events == null)
				events = new ArrayList<RecordroidPlatformEvent>();
			events.addAll(popped);
		}
		popped = this.mWebPageLoadEventQueue.popAll(isUrgentPop);
		if(popped != null) {
			if(events == null)
				events = new ArrayList<RecordroidPlatformEvent>();
			events.addAll(popped);
		}
		popped = this.mActivityLaunchEventQueue.popAll(isUrgentPop);
		if(popped != null) {
			if(events == null)
				events = new ArrayList<RecordroidPlatformEvent>();
			events.addAll(popped);
		}
		return events;
	}

	public void onViewInputEvent(long startTimeMS) {
		long startTimeUS = startTimeMS * 1000;
		long endTimeUS = RecordroidTimer.getUptimeNS().toUS();

		this.mViewInputEventQueue.push(startTimeUS, endTimeUS);
	}

	public void onWebPageLoadEvent_Start(String url) {
		long startTimeUS = RecordroidTimer.getUptimeNS().toUS();
		this.mWebPageLoadEventQueue.pushStart(startTimeUS, url);
	}

	public void onWebPageLoadEvent_Finish(String url) {
		long endTimeUS = RecordroidTimer.getUptimeNS().toUS();
		this.mWebPageLoadEventQueue.pushEnd(endTimeUS, url);
	}

	public void onActivityLaunchEvent(long responseTimeUS, 
		String componentName) {
		long endTimeUS = RecordroidTimer.getUptimeNS().toUS();
		this.mActivityLaunchEventQueue.push(endTimeUS, responseTimeUS,
			componentName);
	}

	public void onActivityPauseEvent() {
		this.mActivityPauseEventQueue.push();
	}

	public void onViewShortClickEvent() {
		this.mViewShortClickEventQueue.push();
	}

	public void onViewLongClickEvent() {
		this.mViewLongClickEventQueue.push();
	}
}

abstract class PlatformRawEventQueue {
	protected Lock mLock = new ReentrantLock();
	protected ArrayList<PlatformRawEvent> mEvents;

	public PlatformRawEventQueue() {
		this.mEvents = new ArrayList<PlatformRawEvent>();
	}

	public ArrayList<RecordroidPlatformEvent> popAll(boolean isUrgentPop) {
		this.mLock.lock();
		ArrayList<RecordroidPlatformEvent> platformEvents = null;
		try {
			for(final Iterator<PlatformRawEvent> iter = this.mEvents.iterator();
					iter.hasNext() == true;) {
				PlatformRawEvent e = iter.next();
				if(e.isComplete(isUrgentPop) == true) {
					if(platformEvents == null)
						platformEvents = new ArrayList<RecordroidPlatformEvent>();
					platformEvents.add(e.toPlatformEvent());
					iter.remove();
				}
			}
		} finally {
			this.mLock.unlock();
		}
		return platformEvents;
	}
}

abstract class PlatformRawEvent {
	public long startTimeUS;
	public long endTimeUS;

	protected PlatformRawEvent(long startTimeUS, long endTimeUS) {
		this.startTimeUS = startTimeUS;
		this.endTimeUS = endTimeUS;
	}
	protected PlatformRawEvent(long startTimeUS) {
		this(startTimeUS, -1);
	}
	public void setEndTime(long endTimeUS) {
		this.endTimeUS = endTimeUS;
	}
	abstract public RecordroidPlatformEvent toPlatformEvent();
	abstract public boolean isComplete(boolean isUrgentPop);
}

class ViewInputEventQueue extends PlatformRawEventQueue {	
	public void push(long startTimeUS, long endTimeUS) {
		if(RecordroidPlatformEventQueue.get().isEnabled() == false) return;
		
		this.mLock.lock();
		try {
			boolean isDone = false;
			for(PlatformRawEvent e: this.mEvents) {
				Event event = (Event)e;
				if(event.startTimeUS == startTimeUS) {
					// found existing raw event: update attributes to the event
					event.setEndTime(endTimeUS);
					isDone = true;
				}
				if(isDone == true)
					break;
			}
			if(isDone == false) {
				// cannot find existing event: make new raw event
				Event newEvent = new Event(startTimeUS, endTimeUS);
				this.mEvents.add(newEvent);
			}
		} finally {
			this.mLock.unlock();
		}
	}

	class Event extends PlatformRawEvent {
		public Event(long startTimeUS, long endTimeUS) {
			super(startTimeUS, endTimeUS);
		}

		@Override
		public void setEndTime(long endTimeUS) {
			super.setEndTime(endTimeUS);
		}

		@Override
		public RecordroidPlatformEvent toPlatformEvent() {
			return RecordroidPlatformEvent.makeViewInputEvent(this.endTimeUS,
				(int)(this.endTimeUS - this.startTimeUS));
		}

		@Override
		public boolean isComplete(boolean isUrgentPop) {
			if(isUrgentPop == true)
				return true;
			final long MSECS_TO_US = 1000;
			final long THRESHOLD_US = MSECS_TO_US * 100;
			long currentUptimeUS = RecordroidTimer.getUptimeNS().toUS();
			if(currentUptimeUS - this.endTimeUS < THRESHOLD_US)
				// below 100 ms
				return false;
			else
				// over 100 ms
				return true;
		}
	}
}

class WebPageLoadEventQueue extends PlatformRawEventQueue {
	public void pushStart(long startTimeUS, String url) {
		if(RecordroidPlatformEventQueue.get().isEnabled() == false) return;
		
		this.mLock.lock();
		try {
			// make new raw event
			Event newEvent = new Event(startTimeUS, url);
			this.mEvents.add(newEvent);
		} finally {
			this.mLock.unlock();
		}
	}

	public void pushEnd(long endTimeUS, String url) {
		if(RecordroidPlatformEventQueue.get().isEnabled() == false) return;
		
		this.mLock.lock();
		try {
			for(PlatformRawEvent e: this.mEvents) {
				Event event = (Event)e;
				if(event.url.compareTo(url) == 0) {
					// update attributes to the event
					event.setEndTime(endTimeUS);
				}
			}
		} finally {
			this.mLock.unlock();
		}
	}

	class Event extends PlatformRawEvent {
		public String url;
		private boolean mIsComplete;
		public Event(long startTimeUS, String url) {
			super(startTimeUS);
			this.url = url;
			this.mIsComplete = false;
		}

		@Override
		public void setEndTime(long endTimeUS) {
			super.setEndTime(endTimeUS);
			this.mIsComplete = true;
		}

		@Override
		public RecordroidPlatformEvent toPlatformEvent() {
			return RecordroidPlatformEvent.makeWebPageLoadEvent(this.endTimeUS,
				(int)(this.endTimeUS - this.startTimeUS));
		}

		@Override
		public boolean isComplete(boolean isUrgentPop) {
			// It has no relation with 'urgent pop'.
			return this.mIsComplete;
		}
		
	}
}

class ActivityLaunchEventQueue extends PlatformRawEventQueue {	
	public void push(long endTimeUS, long responseTimeUS, String componentName) {
		if(RecordroidPlatformEventQueue.get().isEnabled() == false) return;
		
		this.mLock.lock();
		try {
			Event newEvent = new Event(endTimeUS - responseTimeUS, endTimeUS, componentName);
			this.mEvents.add(newEvent);
		
		} finally {
			this.mLock.unlock();
		}
	}

	class Event extends PlatformRawEvent {
		public String componentName;
		public Event(long startTimeUS, long endTimeUS, String componentName) {
			super(startTimeUS, endTimeUS);
			this.componentName = componentName;
		}

		@Override
		public void setEndTime(long endTimeUS) {
			super.setEndTime(endTimeUS);
		}

		@Override
		public RecordroidPlatformEvent toPlatformEvent() {
			return RecordroidPlatformEvent.makeActivityLaunchEvent(this.endTimeUS,
				(int)(this.endTimeUS - this.startTimeUS),
				componentName.hashCode());
		}

		@Override
		public boolean isComplete(boolean isUrgentPop) {
			return true;
		}
	}
}

abstract class InstantPlatformRawEventQueue extends PlatformRawEventQueue {
	public void push() {
		if(RecordroidPlatformEventQueue.get().isEnabled() == false) return;
		
		this.mLock.lock();
		try {
			InstantPlatformRawEvent newEvent = this.makeEvent();
		} finally {
			this.mLock.unlock();
		}
	}
	abstract protected InstantPlatformRawEvent makeEvent();
}

abstract class InstantPlatformRawEvent extends PlatformRawEvent {
	protected InstantPlatformRawEvent () {
		super(RecordroidTimer.getUptimeNS().toUS());
	}
	abstract public RecordroidPlatformEvent toPlatformEvent();
	public boolean isComplete(boolean isUrgentPop) {
		return true;
	}
}

class ActivityPauseEventQueue extends InstantPlatformRawEventQueue {
	protected InstantPlatformRawEvent makeEvent() {
		return new ActivityPauseEvent();
	}
}
class ActivityPauseEvent extends InstantPlatformRawEvent {
	public RecordroidPlatformEvent toPlatformEvent() {
		return RecordroidPlatformEvent.makeActivityPauseEvent(this.startTimeUS);
	}
}
class ViewShortClickEventQueue extends InstantPlatformRawEventQueue {
	protected InstantPlatformRawEvent makeEvent() {
		return new ViewShortClickEvent();
	}
}
class ViewShortClickEvent extends InstantPlatformRawEvent {
	public RecordroidPlatformEvent toPlatformEvent() {
		return  RecordroidPlatformEvent.makeViewShortClickEvent(this.startTimeUS);
	}
}

class ViewLongClickEventQueue extends InstantPlatformRawEventQueue {
	protected InstantPlatformRawEvent makeEvent() {
		return new ViewLongClickEvent();
	}
}
class ViewLongClickEvent extends InstantPlatformRawEvent {
	public RecordroidPlatformEvent toPlatformEvent() {
		return RecordroidPlatformEvent.makeViewLongClickEvent(this.startTimeUS);
	}
}

