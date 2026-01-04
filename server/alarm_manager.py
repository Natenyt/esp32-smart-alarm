"""
Alarm Manager - state machine and scheduler for alarm logic
"""
import asyncio
from datetime import datetime, timedelta
from typing import Optional, Callable, Awaitable, TYPE_CHECKING

from database import db, Alarm, AlarmState, User
from qr_handler import generate_qr_token, generate_qr_image

if TYPE_CHECKING:
    from telegram_bot import TelegramBot


class AlarmManager:
    """
    Manages alarm state and scheduling.
    This is the brain of the system.
    """
    
    def __init__(self):
        self._check_task: Optional[asyncio.Task] = None
        self._telegram_bot: Optional["TelegramBot"] = None
        
        # Current state for ESP32 polling
        self._device_state = "IDLE"  # "IDLE" or "ALARM_RINGING"
        self._current_qr_token: Optional[str] = None
        self._current_ringing_alarm_id: Optional[int] = None
        self._current_ringing_user_id: Optional[int] = None
    
    def set_telegram_bot(self, bot: "TelegramBot"):
        """Set telegram bot reference for notifications."""
        self._telegram_bot = bot
    
    async def start(self):
        """Start the alarm checking background task."""
        if self._check_task is None:
            self._check_task = asyncio.create_task(self._check_loop())
            print("Alarm manager started")
    
    async def stop(self):
        """Stop the alarm checking background task."""
        if self._check_task:
            self._check_task.cancel()
            try:
                await self._check_task
            except asyncio.CancelledError:
                pass
            self._check_task = None
            print("Alarm manager stopped")
    
    async def _check_loop(self):
        """Background loop that checks for alarms to trigger."""
        while True:
            try:
                await self._check_alarms()
                await asyncio.sleep(1)  # Check every second
            except asyncio.CancelledError:
                break
            except Exception as e:
                print(f"Alarm check error: {e}")
                await asyncio.sleep(5)
    
    async def _check_alarms(self):
        """Check if any scheduled alarm should trigger now."""
        now = datetime.now()
        
        # Get all scheduled alarms
        scheduled = db.get_scheduled_alarms()
        
        for alarm in scheduled:
            if alarm.trigger_time <= now:
                await self._trigger_alarm(alarm)
                break  # Only trigger one at a time
        
        # Check for expired ringing alarm (10 minute timeout)
        ringing = db.get_ringing_alarm()
        if ringing and ringing.triggered_at:
            elapsed = (now - ringing.triggered_at).total_seconds()
            if elapsed > 600:  # 10 minutes
                await self._expire_alarm(ringing)
        
        # Check for unnotified completed alarms (for separate process mode)
        # Only if we have a bot to send messages
        if self._telegram_bot:
            await self._check_notifications()

    async def _check_notifications(self):
        """Check for completed alarms that haven't been notified."""
        unnotified = db.get_unnotified_alarms()
        
        for alarm in unnotified:
            if not alarm.user_id:
                continue
                
            print(f"Sending success notification for alarm {alarm.id}")
            user = db.get_user_by_id(alarm.user_id) if hasattr(db, 'get_user_by_id') else None
            
            if user:
                try:
                    await self._telegram_bot.send_alarm_stopped(user, alarm)
                    # Only mark as sent if successful (or if we want to avoid retry loops on error?)
                    # Let's mark it to avoid infinite loops if network is down
                    db.mark_notification_sent(alarm.id)
                except Exception as e:
                    print(f"Failed to send notification: {e}")
                    # Force mark as sent to avoid spam/loops, or implement retry logic later
                    # For now, mark as sent so we don't crash
                    db.mark_notification_sent(alarm.id)

    
    async def _trigger_alarm(self, alarm: Alarm):
        """Trigger an alarm - start ringing."""
        print(f"=" * 50)
        print(f"TRIGGERING ALARM {alarm.id} for user {alarm.user_id}")
        print(f"=" * 50)
        
        # Get user for this alarm
        user = db.get_user_by_id(alarm.user_id) if hasattr(db, 'get_user_by_id') else None
        
        if not user:
            print(f"ERROR: User {alarm.user_id} not found!")
            return
        
        print(f"User found: {user.name} (chat_id: {user.chat_id})")
        
        # Generate QR token and image
        token = generate_qr_token()
        qr_image = generate_qr_image(token)
        print(f"Generated QR token: {token}")
        print(f"QR image size: {len(qr_image)} bytes")
        
        # Update database FIRST (so alarm still rings even if Telegram fails)
        now = datetime.now()
        db.update_alarm_state(
            alarm.id,
            AlarmState.RINGING,
            qr_token=token,
            triggered_at=now
        )
        print("Database updated: State = RINGING")
        
        # Update device state
        self._device_state = "ALARM_RINGING"
        self._current_qr_token = token
        self._current_ringing_alarm_id = alarm.id
        self._current_ringing_user_id = alarm.user_id
        print("Device state updated: ALARM_RINGING")
        
        # Send QR code via Telegram (with error handling)
        if self._telegram_bot and user:
            try:
                print(f"Attempting to send QR code to Telegram chat {user.chat_id}...")
                await self._telegram_bot.send_alarm_qr(user, qr_image)
                print("✅ QR CODE SENT SUCCESSFULLY!")
            except Exception as e:
                print(f"❌ FAILED to send QR code to Telegram: {e}")
                print(f"Alarm is still RINGING - user can scan from device or check logs")
                # Don't crash - alarm should still work via device scanning
        else:
            if not self._telegram_bot:
                print("WARNING: Telegram bot not set")
            if not user:
                print("WARNING: User not found")
        
        print(f"=" * 50)
    
    async def _expire_alarm(self, alarm: Alarm):
        """Expire an alarm that wasn't stopped in time."""
        print(f"Expiring alarm {alarm.id}")
        
        db.update_alarm_state(alarm.id, AlarmState.EXPIRED)
        
        # Reset device state
        self._device_state = "IDLE"
        self._current_qr_token = None
        self._current_ringing_alarm_id = None
        self._current_ringing_user_id = None
    
    def set_alarm(self, user_id: int, hour: int, minute: int) -> Alarm:
        """
        Set a new alarm for the given user and time.
        If time has passed today, schedule for tomorrow.
        """
        now = datetime.now()
        trigger_time = now.replace(hour=hour, minute=minute, second=0, microsecond=0)
        
        # If time has passed today, schedule for tomorrow
        if trigger_time <= now:
            trigger_time += timedelta(days=1)
        
        # Cancel any existing scheduled alarms for this user
        db.cancel_user_scheduled_alarms(user_id)
        
        # Create new alarm
        alarm = db.create_alarm(user_id, trigger_time)
        print(f"Alarm set for user {user_id} at {trigger_time}")
        
        return alarm
    
    def get_user_status(self, user_id: int) -> dict:
        """Get current alarm status for a specific user."""
        # Check if this user's alarm is ringing
        if self._current_ringing_user_id == user_id:
            ringing = db.get_alarm(self._current_ringing_alarm_id)
            if ringing:
                return {
                    "state": "ringing",
                    "alarm_id": ringing.id,
                    "triggered_at": ringing.triggered_at.isoformat() if ringing.triggered_at else None
                }
        
        # Check for scheduled alarms
        scheduled = db.get_user_scheduled_alarms(user_id)
        if scheduled:
            next_alarm = scheduled[0]
            return {
                "state": "scheduled",
                "alarm_id": next_alarm.id,
                "trigger_time": next_alarm.trigger_time.isoformat()
            }
        
        return {
            "state": "idle",
            "alarm_id": None
        }
    
    def cancel_user_alarm(self, user_id: int) -> bool:
        """Cancel any scheduled alarms for a user."""
        count = db.cancel_user_scheduled_alarms(user_id)
        return count > 0
    
    def get_device_state(self) -> str:
        """Get current state for ESP32 polling - reads from database."""
        ringing = db.get_ringing_alarm()
        if ringing:
            # Update in-memory state from database
            self._device_state = "ALARM_RINGING"
            self._current_ringing_alarm_id = ringing.id
            self._current_ringing_user_id = ringing.user_id
            self._current_qr_token = ringing.qr_token
            return "ALARM_RINGING"
        else:
            self._device_state = "IDLE"
            return "IDLE"
    
    async def process_scan(self, scanned_token: str) -> dict:
        """
        Process a QR scan from the device.
        Returns action for the device to take.
        """
        if self._device_state != "ALARM_RINGING":
            return {"action": "CONTINUE", "message": "No active alarm"}
        
        if not self._current_qr_token:
            return {"action": "CONTINUE", "message": "No QR token set"}
        
        # Check if token matches
        if scanned_token.strip() == self._current_qr_token.strip():
            # Success! Stop the alarm
            alarm_id = self._current_ringing_alarm_id
            user_id = self._current_ringing_user_id
            
            if alarm_id:
                ringing = db.get_alarm(alarm_id)
                if ringing:
                    now = datetime.now()
                    wake_time = None
                    if ringing.triggered_at:
                        wake_time = int((now - ringing.triggered_at).total_seconds())
                    
                    db.update_alarm_state(
                        alarm_id,
                        AlarmState.COMPLETED,
                        stopped_at=now,
                        wake_time_seconds=wake_time
                    )
                    
                    # Reset device state
                    self._device_state = "IDLE"
                    self._current_qr_token = None
                    self._current_ringing_alarm_id = None
                    self._current_ringing_user_id = None
                    
                    # Send success message via Telegram
                    if self._telegram_bot and user_id:
                        user = db.get_user_by_id(user_id) if hasattr(db, 'get_user_by_id') else None
                        alarm = db.get_alarm(alarm_id)
                        if user and alarm:
                            await self._telegram_bot.send_alarm_stopped(user, alarm)
                    
                    return {
                        "action": "STOP",
                        "message": "Alarm stopped successfully",
                        "wake_time_seconds": wake_time
                    }
        
        return {"action": "CONTINUE", "message": "Invalid QR code"}


# Singleton instance
alarm_manager = AlarmManager()
