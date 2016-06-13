package com.android.server.recordroid;

import android.app.Notification;
import android.app.NotificationManager;
import android.content.Context;
import android.os.IRecordroidService;
import android.os.Process;
import android.os.UserHandle;
import android.util.Log;

import java.util.ArrayList;

public class RecordroidService extends IRecordroidService.Stub 
	implements RecordroidEventListener, USBMessageListener, EventReplayerStateListener {

	private static final String TAG = "RecordroidService";
	private static final int SELF_PORT = 33001;
	private static final int NOTI_ID = 7777;

	private RecordroidServiceState mServiceState;

	private PlatformEventPoller mPlatformEventPoller;
	private KernelInputEventPoller mKernelInputEventPoller;
	private EventReplayer mEventReplayer;
	private USBConnector mUSBConnector;
	private InitializationWorker mInitWorker;
	private Context mContext;

	public RecordroidService(Context context) {
		super();
		this.mContext = context;

		// Initialize state as idle
		this.mServiceState = RecordroidServiceState.makeIdle();
		
		// Initialize each pollers
		this.mPlatformEventPoller = new PlatformEventPoller();
		this.mKernelInputEventPoller = new KernelInputEventPoller();
		this.mEventReplayer = new EventReplayer();

		// Initialize USB connector and start its threads
		this.mUSBConnector = USBConnector.server(SELF_PORT);
		this.mUSBConnector.start();

		// Add USBMessageListeners
		this.mUSBConnector.addListener(this);

		// Run initialization worker
		this.mInitWorker = new InitializationWorker();
		this.mInitWorker.start();
		
		Log.i(TAG, "Recordroid Service: initialization done");
	}

	// Update notification
	private boolean updateNotification(int newStateType) {
		final String TITLE = "Recordroid";
		switch(newStateType) {
		case RecordroidServiceState.ServiceStateType.IDLE: {
			return this.displayNotification(TITLE, "Idle",
				com.android.internal.R.drawable.stat_notify_error);
		}
		case RecordroidServiceState.ServiceStateType.RECORDING: {
			return this.displayNotification(TITLE, "Now Recording...",
				com.android.internal.R.drawable.stat_notify_sync);
		}
		case RecordroidServiceState.ServiceStateType.PREPARING_TO_REPLAY: {
			return this.displayNotification(TITLE, "Preparing to replay...",
				com.android.internal.R.drawable.stat_notify_sync);
		}
		case RecordroidServiceState.ServiceStateType.REPLAYING: {
			return this.displayNotification(TITLE, "Now Replaying...",
				com.android.internal.R.drawable.stat_notify_sync);
		}
		default: {
			return false;
		}
		}
	}

	private boolean displayNotification(String title,
			String tickerText, int iconId) {
		NotificationManager notiManager = (NotificationManager) this.mContext
				.getSystemService(Context.NOTIFICATION_SERVICE);
		try {
			notiManager.cancel(NOTI_ID);
		} catch(NullPointerException e) {
			return false;
		}
		
		Notification noti = new Notification();
		noti.icon = iconId;
        noti.tickerText = tickerText;
        noti.flags |= Notification.FLAG_NO_CLEAR;
        noti.setLatestEventInfo(mContext, title, tickerText, null);
		
		notiManager.notifyAsUser(null, NOTI_ID, noti,
                UserHandle.ALL);
		return true;
	}

	// Command handlers
	private void turnOnRecording() {
		this.mPlatformEventPoller.addListener(this);
		this.mPlatformEventPoller.startThread();
		this.mKernelInputEventPoller.addListener(this);
		this.mKernelInputEventPoller.startThread();
		this.setServiceState(RecordroidServiceState.makeRecording());
	}

	private void turnOffRecording() {
		this.mPlatformEventPoller.killThread();
		this.mPlatformEventPoller.removeListener(this);
		this.mKernelInputEventPoller.killThread();
		this.mKernelInputEventPoller.removeListener(this);
		this.setServiceState(RecordroidServiceState.makeIdle());
	}

	private void turnOnReplaying(RecordroidCommand.ReplayingOnFields fields) {
		this.mPlatformEventPoller.addListener(this.mEventReplayer);
		this.mPlatformEventPoller.addListener(this);
		this.mPlatformEventPoller.startThread();
		int defaultReplayBufferSize = fields.replayBufferSize;
		int maxSleepTimeMS = fields.maxSleepTimeMS;
		this.mEventReplayer.addListener(this);
		this.mEventReplayer.startThread(defaultReplayBufferSize, maxSleepTimeMS);
		this.setServiceState(RecordroidServiceState.makePreparingToReplay());
	}

	private void turnOffReplaying() {
		this.mPlatformEventPoller.killThread();
		this.mPlatformEventPoller.removeListener(this);
		this.mPlatformEventPoller.removeListener(this.mEventReplayer);
		this.mEventReplayer.removeListener(this);
		this.mEventReplayer.killThread();
		this.setServiceState(RecordroidServiceState.makeIdle());
	}

	private void fillReplayBuffer(RecordroidCommand.FillReplayBufferFields fields) {
		boolean isNextExists = fields.isNextExists;
		int numEvents = fields.numEvents;
		long sn = fields.sn;
		this.mEventReplayer.startFillingBuffer(isNextExists, numEvents, sn);
	}

	private void skipWaitingInReplay() {
		this.mEventReplayer.skipWaitingInReplay();
	}

	private RecordroidServiceState notifyServiceState() {
		int serviceStateType = this.getServiceState().serviceStateType;
		if(serviceStateType == RecordroidServiceState.ServiceStateType.REPLAYING
			|| serviceStateType == RecordroidServiceState.ServiceStateType.PREPARING_TO_REPLAY) {
			this.updateReplayingFields();
		}
		return this.getServiceState();
	}

	private void updateReplayingFields() {
		this.mEventReplayer.updateReplayingFields();
	}

	private void setServiceState(RecordroidServiceState newServiceState) {
		int oldStateType, newStateType;
		if(this.mServiceState != null)
			oldStateType = this.mServiceState.serviceStateType;
		else
			oldStateType = -1;
		newStateType = newServiceState.serviceStateType;
		this.mServiceState = newServiceState;
		if(oldStateType != newStateType) {
			this.onChangedServiceStateType(oldStateType, newStateType);
		}
	}

	private RecordroidServiceState getServiceState() {
		return this.mServiceState;
	}

	private void onChangedServiceStateType(int oldStateType, 
		int newStateType) {
		updateNotification(newStateType);
	}
	
	// Implements RecordroidEventListener
	public void onPollEvent(RecordroidEvent event) {
		// Send a event message to controller
		this.mUSBConnector.sendMessage(event);
	}

	// Implements USBMessageListener
	public void onUSBMessage(ArrayList<RecordroidMessage> messages) {
		// Listen command from target via USB interface and handle the command.
		if(messages == null)
			return;
		//Log.d(TAG, "onUSBMessage start: " + messages.size());
		for(RecordroidMessage msg: messages) {
			if(msg instanceof RecordroidCommand) {
				// Handling commands
				RecordroidCommand command = (RecordroidCommand)msg;
				int cmdType = command.commandType;
				RecordroidServiceState state = null;
				//Log.d(TAG, "USBMsg command: " + cmdType);
				// Do action according to type
				switch(cmdType) {
				case RecordroidCommand.CommandType.RECORDING_ON: {
					// Turn on recording
					this.turnOnRecording();
					state = this.notifyServiceState();
				}
				break;
				case RecordroidCommand.CommandType.RECORDING_OFF: {
					// Turn off recording
					this.turnOffRecording();
					state = this.notifyServiceState();
				}
				break;
				case RecordroidCommand.CommandType.REPLAYING_ON: {
					// Turn on replaying
					RecordroidCommand.ReplayingOnFields fields = command.replayingOnFields;
					this.turnOnReplaying(fields);
					state = this.notifyServiceState();
				}
				break;
				case RecordroidCommand.CommandType.REPLAYING_OFF: {
					// Turn off replaying
					this.turnOffReplaying();
					state = this.notifyServiceState();
				}
				break;
				case RecordroidCommand.CommandType.REQUEST_STATE: {
					// Request state
					state = this.notifyServiceState();
				}
				break;
				case RecordroidCommand.CommandType.FILL_REPLAY_BUFFER: {
					// Fill replay buffer
					RecordroidCommand.FillReplayBufferFields fields = command.fillReplayBufferFields;
					this.fillReplayBuffer(fields);
				}
				break;
				case RecordroidCommand.CommandType.SKIP_WAITING_IN_REPLAY: {
					this.skipWaitingInReplay();
				}
				break;
				}
					// Report its state to target
					if(state != null) {
						this.mUSBConnector.sendMessage(state);
				}
			}
			else if(msg instanceof RecordroidKernelInputEvent) {
				// Handling kernel input events
				// Fetch the event to event player's queue
				//Log.d(TAG, "USBMsg kernel input event");
				RecordroidKernelInputEvent event = (RecordroidKernelInputEvent)msg;
				this.mEventReplayer.fetchKernelInputEvent(event);
			}
			else if(msg instanceof RecordroidPlatformEvent) {
				// Handling platform events
				// Fetch the event to event player's queue
				//Log.d(TAG, "USBMsg platform event");
				RecordroidPlatformEvent event = (RecordroidPlatformEvent)msg;
				this.mEventReplayer.fetchPlatformEvent(event);
			}
		}
		//Log.d(TAG, "onUSBMessage end");
	}

	public void willDoUSBConnectorRoutine() {
		// Ignore
	}
	
	public void didUSBConnectorRoutine() {
		// Ignore
	}

	// Implements EventReplayerStateListener
	public void didUpdateReplayingFields(long requiredSN, 
		long runningSN, int presentReplayBufferIndex,
		int presentReplayBufferSize) {
		this.setServiceState(RecordroidServiceState.makeReplaying(requiredSN,
			runningSN, presentReplayBufferIndex, presentReplayBufferSize));
	}

	public void didFinishReplaying() {
		this.turnOffReplaying();
	}

	// Implements IRecordroidService.Stub
	public void onViewInputEvent(long startTimeMS) {
		RecordroidPlatformEventQueue.get().onViewInputEvent(startTimeMS);
	}
	public void onWebPageLoadEvent_Start(String url) {
		RecordroidPlatformEventQueue.get().onWebPageLoadEvent_Start(url);
	}
	public void onWebPageLoadEvent_Finish(String url) {
		RecordroidPlatformEventQueue.get().onWebPageLoadEvent_Finish(url);
	}
  	public void onActivityLaunchEvent(long responseTimeUS, String componentName) {
		RecordroidPlatformEventQueue.get().onActivityLaunchEvent(responseTimeUS, componentName);
  	}

	public void onActivityPauseEvent() {
		RecordroidPlatformEventQueue.get().onActivityPauseEvent();
	}

	public void onViewShortClickEvent() {
		RecordroidPlatformEventQueue.get().onViewShortClickEvent();
	}

	public void onViewLongClickEvent() {
		RecordroidPlatformEventQueue.get().onViewLongClickEvent();
	}
	
	class InitializationWorker extends Thread {
		@Override
		public void run() {
			boolean succeed = false;
			while(succeed == false) {
				succeed = updateNotification(RecordroidServiceState.ServiceStateType.IDLE);
				try {
					Thread.sleep(1000);
				} catch (InterruptedException e) {
				}
			}
		}
	}
}
