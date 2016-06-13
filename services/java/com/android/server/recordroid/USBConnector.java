package com.android.server.recordroid;

import android.util.Log;

import java.io.EOFException;
import java.io.IOException;
import java.io.ObjectInputStream;
import java.io.ObjectOutputStream;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.ArrayList;
import java.util.concurrent.locks.Lock;
import java.util.concurrent.locks.ReentrantLock;

// It abstracts USB communication behaviors between Host PC and Android target device.
public class USBConnector {
	private static final String TAG = "USBConnector";
	private static final int DEFAULT_SLEEP_MS = 2000;

	private Worker mWorker;
	private boolean mIsServer;
	private int mPort;
	private int mSleepMS;

	private MessageBuffer mSendBuffer;
	private ArrayList<USBMessageListener> mListeners;

	private USBConnector(boolean isServer, int port, int sleepMS) {
		this.mIsServer = isServer;
		this.mPort = port;
		this.mSleepMS = sleepMS;

		this.mListeners = new ArrayList<USBMessageListener>();
		this.mSendBuffer = new MessageBuffer();
	}

	public static USBConnector server(int selfPort, int sleepMS) {
		return new USBConnector(true, selfPort, sleepMS);
	}

	public static USBConnector client(int targetPort, int sleepMS) {
		return new USBConnector(false, targetPort, sleepMS);
	}

	public static USBConnector server(int selfPort) {
		return new USBConnector(true, selfPort, DEFAULT_SLEEP_MS);
	}

	public static USBConnector client(int targetPort) {
		return new USBConnector(false, targetPort, DEFAULT_SLEEP_MS);
	}

	public void start() {
		this.stop();
		this.mWorker = new Worker(this.mIsServer, this.mPort, this.mSleepMS);
		this.mWorker.start();
	}

	public void stop() {
		if (this.mWorker != null) {
			this.mWorker.kill();
			try {
				this.mWorker.join();
			} catch (InterruptedException e) {
				e.printStackTrace();
			}
		}
	}

	public void sendMessage(RecordroidMessage message) {
		if (this.mWorker != null && this.mWorker.isRunning() == true) {
			this.mSendBuffer.add(message);
		}
	}

	public int getSendBufferLength() {
		return this.mSendBuffer.getLength();
	}

	public void addListener(USBMessageListener listener) {
		this.mListeners.add(listener);
	}

	public void removeListener(USBMessageListener listener) {
		this.mListeners.remove(listener);
	}

	// Listen messages incoming from target and send messages of buffer to
	// target periodically
	class Worker extends Thread {
		private static final String THREAD_NAME = "USBConnectorThread";
		private int mPort;
		private int mSleepMS;
		private boolean mIsRunning;
		private boolean mIsServer;

		public Worker(boolean isServer, int port, int sleepMS) {
			super(THREAD_NAME);
			this.mIsServer = isServer;
			this.mPort = port;
			this.mSleepMS = sleepMS;
			this.mIsRunning = false;
		}

		public boolean isRunning() {
			return this.mIsRunning;
		}

		public void run() {
			if (this.mIsServer) {
				this.runServer();
			} else {
				this.runClient();
			}
		}

		@SuppressWarnings("unchecked")
		private void runServer() {
			this.mIsRunning = true;
			DebugLog.i(TAG, "USBServerThread is started");

			ObjectInputStream inStream = null;
			ObjectOutputStream outStream = null;
			ServerSocket self = null;
			Socket target = null;

			try {
				// Initialize this server(self)
				int selfPort = this.mPort;
				self = new ServerSocket(selfPort);
				self.setSoTimeout(this.mSleepMS);

				while (this.mIsRunning == true) {
					for (USBMessageListener listener : mListeners) {
						listener.willDoUSBConnectorRoutine();
					}

					try {
						// establish connection from target
						target = self.accept();
						if (target.getInputStream() == null) {
							DebugLog.e(TAG, "Void input stream!");
							break;
						}
						inStream = new ObjectInputStream(
								target.getInputStream());

						// listen a message from target and broadcast it to
						// listeners
						Object inObject = inStream.readObject();
						ArrayList<RecordroidMessage> inMsg = null;
						if (inObject instanceof ArrayList) {
							inMsg = (ArrayList<RecordroidMessage>) inObject;
							DebugLog.d(TAG, "LISTEN: " + inMsg);
							for (USBMessageListener listener : mListeners) {
								listener.onUSBMessage(inMsg);
							}

							// pop all messages from
							// sen(ArrayList<RecordroidMessage>) inObjectd
							// buffer and send them
							// if
							// possible
							if (target.getOutputStream() == null) {
								DebugLog.e(TAG, "Void output stream!");
								break;
							}
							outStream = new ObjectOutputStream(
									target.getOutputStream());
							final ArrayList<RecordroidMessage> sendMsg = mSendBuffer
									.popAll();
							DebugLog.d(TAG, "RESPOND: " + sendMsg);
							if (sendMsg != null) {
								outStream.writeObject(sendMsg);
								outStream.flush();
							}
						} else {
							// Exception
							DebugLog.e(TAG, "Invalid USB message came!");
						}
					} catch (IOException e) {
						// Ignore
					} catch (ClassNotFoundException e) {
						// Class not found
						DebugLog.e(TAG,
								"Read message error: ClassNotFoundException "
										+ e);
					} finally {
						// Close connection with target
						try {
							if (outStream != null)
								outStream.close();
							if (inStream != null)
								inStream.close();
							if (target != null)
								target.close();
						} catch (IOException e) {
							DebugLog.e(TAG, "Close failure: " + e);
						}
					}

					for (USBMessageListener listener : mListeners) {
						listener.didUSBConnectorRoutine();
					}
				}
				DebugLog.e(TAG, "Invalid USB message came!");
				// Close self
				if (self != null)
					self.close();
			} catch (IOException e) {
				DebugLog.e(TAG, "" + e);
			}

			DebugLog.i(TAG, "USBServerThread is finished");
		}

		private void runClient() {
			this.mIsRunning = true;
			DebugLog.i(TAG, "USBClientThread is started");

			ObjectInputStream inStream = null;
			ObjectOutputStream outStream = null;
			Socket target = null;

			while (this.mIsRunning == true) {
				for (USBMessageListener listener : mListeners) {
					listener.willDoUSBConnectorRoutine();
				}

				// Check if there is any message to be sent in the buffer
				final ArrayList<RecordroidMessage> sendMsg = mSendBuffer
						.popAll();

				// If there is any message, send it and receive messages from
				// target
				if (sendMsg != null) {
					try {
						// Establish connection with target
						int targetPort = this.mPort;
						target = new Socket("localhost", targetPort);
						if (target.getOutputStream() == null) {
							DebugLog.e(TAG, "Void output stream!");
							break;
						}
						outStream = new ObjectOutputStream(
								target.getOutputStream());

						// Send the messages
						outStream.writeObject(sendMsg);
						outStream.flush();
						DebugLog.d(TAG, "SEND: " + sendMsg);

						// Listen messages from target
						if (target.getInputStream() == null) {
							DebugLog.e(TAG, "Void input stream!");
							break;
						}
						inStream = new ObjectInputStream(
								target.getInputStream());
						Object inObject = inStream.readObject();
						if (inObject instanceof ArrayList) {
							@SuppressWarnings("unchecked")
							ArrayList<RecordroidMessage> inMsg = (ArrayList<RecordroidMessage>) inObject;
							DebugLog.d(TAG, "RECEIVE: " + inMsg);
							for (USBMessageListener listener : mListeners) {
								listener.onUSBMessage(inMsg);
							}
						} else {
							DebugLog.e(TAG, "Invalid USB message came!");
						}
						// Close connection with target
						if (outStream != null)
							outStream.close();
						if (inStream != null)
							inStream.close();
						if (target != null)
							target.close();
					} catch (EOFException e) {
						// Ignore
					} catch (ClassNotFoundException e) {
						// Class not found
						DebugLog.e(TAG,
								"Read message error: ClassNotFoundException "
										+ e);
					} catch (IOException e) {
						DebugLog.e(TAG, "" + e);
					} finally {
						// Close connection with target
						try {
							if (outStream != null)
								outStream.close();
							if (inStream != null)
								inStream.close();
							if (target != null)
								target.close();
						} catch (IOException e) {
							DebugLog.e(TAG, "Close failure: " + e);
						}
					}
				}

				for (USBMessageListener listener : mListeners) {
					listener.didUSBConnectorRoutine();
				}

				try {
					Thread.sleep(this.mSleepMS);
				} catch (InterruptedException e) {
					// Ignore
				}
			}

			DebugLog.i(TAG, "USBClientThread is finished");
		}

		public void kill() {
			this.mIsRunning = false;
		}
	}

	class MessageBuffer {
		private Lock mLock = new ReentrantLock();
		private ArrayList<RecordroidMessage> mMessages;

		public MessageBuffer() {
			this.mMessages = new ArrayList<RecordroidMessage>();
		}

		public void add(RecordroidMessage message) {
			this.mLock.lock();
			try {
				this.mMessages.add(message);
			} finally {
				this.mLock.unlock();
			}
		}

		public ArrayList<RecordroidMessage> popAll() {
			ArrayList<RecordroidMessage> result;

			this.mLock.lock();
			try {
				result = this.mMessages;
				this.mMessages = new ArrayList<RecordroidMessage>();
			} finally {
				this.mLock.unlock();
			}

			if (result.size() == 0) {
				result = null;
			}
			return result;
		}

		public int getLength() {
			int length;

			this.mLock.lock();
			try {
				length = this.mMessages.size();
			} finally {
				this.mLock.unlock();
			}
			return length;
		}
	}
}

interface USBMessageListener {
	// 'messages' string includes multiple messages divided by '\n'
	public void onUSBMessage(ArrayList<RecordroidMessage> messages);

	public void willDoUSBConnectorRoutine();

	public void didUSBConnectorRoutine();
}

// Host
//class DebugLog {
//	public static void d(String tag, String message) {
//		// System.out.println("[DEBUG] " + tag + ": " + message);
//	}

//	public static void e(String tag, String message) {
//		// System.out.println("[ERROR] " + tag + ": " + message);
//	}

//	public static void i(String tag, String message) {
//		// System.out.println("[INFO] " + tag + ": " + message);
//	}
//}

// Android Device
class DebugLog {
	public static void d(String tag, String message) {
		//Log.d(tag, message);
	}

	public static void e(String tag, String message) {
		Log.e(tag, message);
	}

	public static void i(String tag, String message) {
		//Log.i(tag, message);
	}
}

