package com.android.server.recordroid;

import java.io.Serializable;

public abstract class RecordroidMessage implements Serializable {
	// Implements Serializable
	private static final long serialVersionUID = 8224767287162748036L;
}

// Recordroid Event Listener
interface RecordroidEventListener {
	public void onPollEvent(RecordroidEvent event);
}

abstract class RecordroidEvent extends RecordroidMessage {
	public long timestampUS;

	abstract public String toStringForTraceFile();

	abstract protected void setFromStringForTraceFile(String strLine);

	// Implements Serializable
	private static final long serialVersionUID = -293539178025237416L;
}

class RecordroidKernelInputEvent extends RecordroidEvent {
	// Fields
	private static final String magicString = "K";
	public int deviceNum;
	public int typeVal;
	public int codeVal;
	public int value;

	private RecordroidKernelInputEvent() {
	}

	// Static Constructors
	public static RecordroidKernelInputEvent make(long tv_sec, long tv_usec,
			int deviceNum, int typeVal, int codeVal, int value) {
		RecordroidKernelInputEvent newEvent = new RecordroidKernelInputEvent();
		final long SECS_TO_USEC = 1000 * 1000;
		newEvent.setFields(tv_sec * SECS_TO_USEC + tv_usec, deviceNum, typeVal,
				codeVal, value);
		return newEvent;
	}

	public static RecordroidKernelInputEvent makeFromStringForTraceFile(
			String strLine) {
		RecordroidKernelInputEvent newEvent = new RecordroidKernelInputEvent();
		newEvent.setFromStringForTraceFile(strLine);
		return newEvent;
	}

	protected void setFields(long timestampUS, int deviceNum, int typeVal,
			int codeVal, int value) {
		this.timestampUS = timestampUS;
		this.deviceNum = deviceNum;
		this.typeVal = typeVal;
		this.codeVal = codeVal;
		this.value = value;
	}

	public static boolean transformableFrom(String strLine) {
		String[] tokens = strLine.split(" ");
		return (tokens[0].compareTo(RecordroidKernelInputEvent.magicString) == 0);
	}

	// Extends RecordroidEvent
	@Override
	protected void setFromStringForTraceFile(String strLine) {
		String[] tokens = strLine.split(" ");
		this.timestampUS = Long.parseLong(tokens[1]);
		this.deviceNum = Integer.parseInt(tokens[2]);
		this.typeVal = Integer.parseInt(tokens[3]);
		this.codeVal = Integer.parseInt(tokens[4]);
		this.value = Integer.parseInt(tokens[5]);
	}

	@Override
	public String toStringForTraceFile() {
		return RecordroidKernelInputEvent.magicString + " " + this.timestampUS
				+ " " + this.deviceNum + " " + this.typeVal + " "
				+ this.codeVal + " " + this.value;
	}

	// Implements Serializable
	private static final long serialVersionUID = -6047318129972559925L;
}

class RecordroidPlatformEvent extends RecordroidEvent {
	private static final String magicString = "P";

	// Type
	static class PlatformEventType {
		public static final int VIEW_INPUT_EVENT = 1;
		public static final int WEB_PAGE_LOAD_EVENT = 2;
		public static final int ACTIVITY_PAUSE_EVENT = 3;
		public static final int ACTIVITY_LAUNCH_EVENT = 4;
		public static final int VIEW_SHORT_CLICK_EVENT = 5;
		public static final int VIEW_LONG_CLICK_EVENT = 6;
		public static final String[] names = { "", "ViewInput", "WebPageLoad",
				"ActivityPause", "ActivityLaunch", "ViewShortClick",
				"ViewLongClick" };
	}

	// Fields
	public int platformEventType;
	public int responseTimeUS;
	public int priv;
	public int secondPriv;

	private RecordroidPlatformEvent() {
	}

	// Static Constructors
	public static RecordroidPlatformEvent makeViewInputEvent(long timestampUS,
			int responseTimeUS) {
		RecordroidPlatformEvent newEvent = new RecordroidPlatformEvent();
		newEvent.setFields(timestampUS, PlatformEventType.VIEW_INPUT_EVENT,
				responseTimeUS, 0, 0);
		return newEvent;
	}

	public static RecordroidPlatformEvent makeWebPageLoadEvent(
			long timestampUS, int responseTimeUS) {
		RecordroidPlatformEvent newEvent = new RecordroidPlatformEvent();
		newEvent.setFields(timestampUS, PlatformEventType.WEB_PAGE_LOAD_EVENT,
				responseTimeUS, 0, 0);
		return newEvent;
	}

	public static RecordroidPlatformEvent makeActivityPauseEvent(long timestampUS) {
		RecordroidPlatformEvent newEvent = new RecordroidPlatformEvent();
		newEvent.setFields(timestampUS, PlatformEventType.ACTIVITY_PAUSE_EVENT,
				0, 0, 0);
		return newEvent;
	}

	public static RecordroidPlatformEvent makeActivityLaunchEvent(
			long timestampUS, int responseTimeUS, int hashedcomponentName) {
		RecordroidPlatformEvent newEvent = new RecordroidPlatformEvent();
		newEvent.setFields(timestampUS,
				PlatformEventType.ACTIVITY_LAUNCH_EVENT, responseTimeUS,
				hashedcomponentName, 0);
		return newEvent;
	}

	public static RecordroidPlatformEvent makeViewShortClickEvent(long timestampUS) {
		RecordroidPlatformEvent newEvent = new RecordroidPlatformEvent();
		newEvent.setFields(timestampUS,
				PlatformEventType.VIEW_SHORT_CLICK_EVENT, 0, 0, 0);
		return newEvent;
	}

	public static RecordroidPlatformEvent makeViewLongClickEvent(long timestampUS) {
		RecordroidPlatformEvent newEvent = new RecordroidPlatformEvent();
		newEvent.setFields(timestampUS,
				PlatformEventType.VIEW_LONG_CLICK_EVENT, 0, 0, 0);
		return newEvent;
	}

	public static RecordroidPlatformEvent makeFromStringForTraceFile(
			String strLine) {
		RecordroidPlatformEvent newEvent = new RecordroidPlatformEvent();
		newEvent.setFromStringForTraceFile(strLine);
		return newEvent;
	}

	// Field Setters
	protected void setFields(long timestampUS, int platformEventType,
			int responseTimeUS, int priv, int secondPriv) {
		this.timestampUS = timestampUS;
		this.platformEventType = platformEventType;
		this.responseTimeUS = responseTimeUS;
		this.priv = priv;
		this.secondPriv = secondPriv;
	}

	public static boolean transformableFrom(String strLine) {
		String[] tokens = strLine.split(" ");
		return (tokens[0].compareTo(RecordroidPlatformEvent.magicString) == 0);
	}

	// Extends RecordroidEvent
	@Override
	protected void setFromStringForTraceFile(String strLine) {
		String[] tokens = strLine.split(" ");
		this.timestampUS = Long.parseLong(tokens[1]);
		this.platformEventType = Integer.parseInt(tokens[2]);
		this.responseTimeUS = Integer.parseInt(tokens[3]);
		this.priv = Integer.parseInt(tokens[4]);
		this.secondPriv = Integer.parseInt(tokens[5]);
	}

	@Override
	public String toStringForTraceFile() {
		return RecordroidPlatformEvent.magicString + " " + this.timestampUS
				+ " " + this.platformEventType + " " + this.responseTimeUS
				+ " " + this.priv + " " + this.secondPriv;
	}

	public String toStringForResponseFile() {

		String timestampStr = "" + this.timestampUS;
		String typeStr = "";
		if (this.platformEventType < PlatformEventType.names.length)
			typeStr = PlatformEventType.names[this.platformEventType];
		String responseTimeStr = "" + this.responseTimeUS;
		String privStr = "" + this.priv;
		String secondPrivStr = "" + this.secondPriv;

		String ret = timestampStr + "\t" + typeStr + "\t" + privStr + "\t"
				+ secondPrivStr + "\t" + responseTimeStr;

		return ret;
	}

	// Implements Serializable
	private static final long serialVersionUID = -8025636410410591660L;
}

class RecordroidCommand extends RecordroidMessage {
	// Type
	class CommandType {
		public static final int RECORDING_ON = 1;
		public static final int RECORDING_OFF = 2;
		public static final int REPLAYING_ON = 3;
		public static final int REPLAYING_OFF = 4;
		public static final int REQUEST_STATE = 5;
		public static final int FILL_REPLAY_BUFFER = 6;
		public static final int SKIP_WAITING_IN_REPLAY = 7;
	}

	// Fields
	public int commandType;
	public FillReplayBufferFields fillReplayBufferFields = null;
	public ReplayingOnFields replayingOnFields = null;

	private RecordroidCommand() {
	}

	// Static constructors
	public static RecordroidCommand makeRecordingOn() {
		RecordroidCommand newCommand = new RecordroidCommand();
		newCommand.setFields(CommandType.RECORDING_ON);
		return newCommand;
	}

	public static RecordroidCommand makeRecordingOff() {
		RecordroidCommand newCommand = new RecordroidCommand();
		newCommand.setFields(CommandType.RECORDING_OFF);
		return newCommand;
	}

	public static RecordroidCommand makeReplayingOn(int replayBufferSize,
			int maxSleepTimeMS) {
		RecordroidCommand newCommand = new RecordroidCommand();
		newCommand.setFields(CommandType.REPLAYING_ON);
		newCommand.setReplayingOnFields(replayBufferSize, maxSleepTimeMS);
		return newCommand;
	}

	public static RecordroidCommand makeReplayingOff() {
		RecordroidCommand newCommand = new RecordroidCommand();
		newCommand.setFields(CommandType.REPLAYING_OFF);
		return newCommand;
	}

	public static RecordroidCommand makeRequestState() {
		RecordroidCommand newCommand = new RecordroidCommand();
		newCommand.setFields(CommandType.REQUEST_STATE);
		return newCommand;
	}

	public static RecordroidCommand makeFillReplayBuffer(long sn,
			int numEvents, boolean isNextExists) {
		RecordroidCommand newCommand = new RecordroidCommand();
		newCommand.setFields(CommandType.FILL_REPLAY_BUFFER);
		newCommand.setFillReplayBufferFields(sn, numEvents, isNextExists);
		return newCommand;
	}

	public static RecordroidCommand makeSkipWaitingInReplay() {
		RecordroidCommand newCommand = new RecordroidCommand();
		newCommand.setFields(CommandType.SKIP_WAITING_IN_REPLAY);
		return newCommand;
	}

	// Field Setters
	protected void setFields(int commandType) {
		this.commandType = commandType;
	}

	protected void setFillReplayBufferFields(long sn, int numEvents,
			boolean isNextExists) {
		this.fillReplayBufferFields = new FillReplayBufferFields(sn, numEvents,
				isNextExists);
	}

	protected void setReplayingOnFields(int replayBufferSize, int maxSleepTimeMS) {
		this.replayingOnFields = new ReplayingOnFields(replayBufferSize,
				maxSleepTimeMS);
	}

	// Implements Serializable
	private static final long serialVersionUID = -3684240374304419741L;

	// Private Fields
	class FillReplayBufferFields implements Serializable {
		public FillReplayBufferFields(long sn, int numEvents,
				boolean isNextExists) {
			this.sn = sn;
			this.numEvents = numEvents;
			this.isNextExists = isNextExists;
		}

		public long sn;
		public int numEvents;
		public boolean isNextExists;

		// implements Serializable
		private static final long serialVersionUID = 8876641731151252167L;
	}

	class ReplayingOnFields implements Serializable {
		public ReplayingOnFields(int replayBufferSize, int maxSleepTimeMS) {
			this.replayBufferSize = replayBufferSize;
			this.maxSleepTimeMS = maxSleepTimeMS;
		}

		public int replayBufferSize;
		public int maxSleepTimeMS;

		// Implements Serializable
		private static final long serialVersionUID = 8642177897685093013L;
	}
}

class RecordroidServiceState extends RecordroidMessage {
	// Type
	class ServiceStateType {
		public static final int RECORDING = 1;
		public static final int PREPARING_TO_REPLAY = 2;
		public static final int REPLAYING = 3;
		public static final int IDLE = 4;
	}

	// Fields
	public int serviceStateType;
	public ReplayingFields replayingFields = null;

	private RecordroidServiceState() {
	}

	// Static constructors
	public static RecordroidServiceState makeRecording() {
		RecordroidServiceState newServiceState = new RecordroidServiceState();
		newServiceState.setFields(ServiceStateType.RECORDING);
		return newServiceState;
	}

	public static RecordroidServiceState makePreparingToReplay() {
		RecordroidServiceState newServiceState = new RecordroidServiceState();
		newServiceState.setFields(ServiceStateType.PREPARING_TO_REPLAY);
		return newServiceState;
	}

	public static RecordroidServiceState makeReplaying(long requiredSN,
			long runningSN, int presentReplayBufferIndex,
			int presentReplayBufferSize) {
		RecordroidServiceState newServiceState = new RecordroidServiceState();
		newServiceState.setFields(ServiceStateType.REPLAYING);
		newServiceState.setReplayingFields(requiredSN, runningSN,
				presentReplayBufferIndex, presentReplayBufferSize);
		return newServiceState;
	}

	public static RecordroidServiceState makeIdle() {
		RecordroidServiceState newServiceState = new RecordroidServiceState();
		newServiceState.setFields(ServiceStateType.IDLE);
		return newServiceState;
	}

	// Field Setters
	protected void setFields(int serviceStateType) {
		this.serviceStateType = serviceStateType;
	}

	protected void setReplayingFields(long requiredSN, long runningSN,
			int presentReplayBufferIndex, int presentReplayBufferSize) {
		this.replayingFields = new ReplayingFields(requiredSN, runningSN,
				presentReplayBufferIndex, presentReplayBufferSize);
	}

	// Implements Serializable
	private static final long serialVersionUID = 4303621857595446675L;

	// Private Fields
	class ReplayingFields implements Serializable {

		public ReplayingFields(long requiredSN, long runningSN,
				int presentReplayBufferIndex, int presentReplayBufferSize) {
			this.requiredSN = requiredSN;
			this.runningSN = runningSN;
			this.presentReplayBufferIndex = presentReplayBufferIndex;
			this.presentReplayBufferSize = presentReplayBufferSize;
		}

		public long requiredSN;
		public long runningSN;
		public int presentReplayBufferIndex;
		public int presentReplayBufferSize;

		// Implements Serializable
		private static final long serialVersionUID = 3009212456262714675L;
	}
}

