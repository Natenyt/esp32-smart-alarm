"""
Telegram Bot - handles user commands and sends notifications
Using aiogram for better stability
"""
import io
import asyncio
from datetime import datetime
from typing import Optional, Dict, Any

from aiogram import Bot, Dispatcher, Router, F
from aiogram.types import Message, BufferedInputFile
from aiogram.filters import Command
from aiogram.enums import ParseMode

from config import config
from database import db, User, Alarm


# Router for handlers
router = Router()

# Store references (set during initialization)
_bot: Optional[Bot] = None
_alarm_manager = None
_user_states: Dict[str, str] = {}  # chat_id -> state

WAITING_FOR_NAME = "waiting_for_name"


def set_alarm_manager(manager):
    """Set alarm manager reference."""
    global _alarm_manager
    _alarm_manager = manager


def _get_user(chat_id: str) -> Optional[User]:
    """Get registered user by chat ID."""
    return db.get_user_by_chat_id(str(chat_id))


@router.message(Command("start"))
async def cmd_start(message: Message):
    """Handle /start command - registration flow."""
    chat_id = str(message.chat.id)
    user = _get_user(chat_id)
    
    if user:
        await message.answer(
            f"👋 Welcome back, *{user.name}*!\n\n"
            "Use `/set HH:MM` to set an alarm.\n"
            "Use `/help` to see all commands.",
            parse_mode=ParseMode.MARKDOWN
        )
    else:
        _user_states[chat_id] = WAITING_FOR_NAME
        await message.answer(
            "🔔 *Welcome to Smart Alarm!*\n\n"
            "I'll help you wake up by making you scan a QR code.\n\n"
            "First, what's your name?",
            parse_mode=ParseMode.MARKDOWN
        )


@router.message(Command("help"))
async def cmd_help(message: Message):
    """Handle /help command."""
    chat_id = str(message.chat.id)
    user = _get_user(chat_id)
    
    if not user:
        await message.answer("Please send /start to register first!")
        return
    
    await message.answer(
        f"🔔 *Smart Alarm Help*\n\n"
        f"Hello, {user.name}!\n\n"
        "*Commands:*\n"
        "• `/set HH:MM` - Set alarm (e.g., /set 07:30)\n"
        "• `/status` - Check current alarm\n"
        "• `/cancel` - Cancel scheduled alarm\n"
        "• `/stats` - View wake-up statistics\n"
        "• `/test` - Test alarm immediately",
        parse_mode=ParseMode.MARKDOWN
    )


@router.message(Command("set"))
async def cmd_set(message: Message):
    """Handle /set command to schedule an alarm."""
    chat_id = str(message.chat.id)
    user = _get_user(chat_id)
    
    if not user:
        await message.answer("Please send /start to register first!")
        return
    
    # Parse time argument
    args = message.text.split()[1:] if message.text else []
    if not args:
        await message.answer(
            "❌ Please specify a time.\n"
            "Example: `/set 07:30`",
            parse_mode=ParseMode.MARKDOWN
        )
        return
    
    time_str = args[0]
    try:
        parts = time_str.split(":")
        hour = int(parts[0])
        minute = int(parts[1]) if len(parts) > 1 else 0
        
        if not (0 <= hour <= 23 and 0 <= minute <= 59):
            raise ValueError("Invalid time range")
        
    except (ValueError, IndexError):
        await message.answer(
            "❌ Invalid time format.\n"
            "Use HH:MM (24-hour format), e.g., `/set 07:30`",
            parse_mode=ParseMode.MARKDOWN
        )
        return
    
    if _alarm_manager:
        alarm = _alarm_manager.set_alarm(user.id, hour, minute)
        
        await message.answer(
            f"✅ *Alarm set!*\n\n"
            f"⏰ Time: {alarm.trigger_time.strftime('%H:%M')}\n"
            f"📅 Date: {alarm.trigger_time.strftime('%A, %B %d')}\n\n"
            f"You'll receive a QR code to scan when it's time to wake up!",
            parse_mode=ParseMode.MARKDOWN
        )
    else:
        await message.answer("❌ Alarm manager not available.")


@router.message(Command("status"))
async def cmd_status(message: Message):
    """Handle /status command."""
    chat_id = str(message.chat.id)
    user = _get_user(chat_id)
    
    if not user:
        await message.answer("Please send /start to register first!")
        return
    
    if _alarm_manager:
        status = _alarm_manager.get_user_status(user.id)
        
        if status["state"] == "ringing":
            await message.answer(
                "🔔 *ALARM IS RINGING!*\n\n"
                "Scan the QR code to stop it!",
                parse_mode=ParseMode.MARKDOWN
            )
        elif status["state"] == "scheduled":
            trigger_time = datetime.fromisoformat(status["trigger_time"])
            await message.answer(
                f"⏰ *Alarm Scheduled*\n\n"
                f"Time: {trigger_time.strftime('%H:%M')}\n"
                f"Date: {trigger_time.strftime('%A, %B %d')}",
                parse_mode=ParseMode.MARKDOWN
            )
        else:
            await message.answer(
                "😴 *No alarm set*\n\n"
                "Use `/set HH:MM` to set one.",
                parse_mode=ParseMode.MARKDOWN
            )


@router.message(Command("cancel"))
async def cmd_cancel(message: Message):
    """Handle /cancel command."""
    chat_id = str(message.chat.id)
    user = _get_user(chat_id)
    
    if not user:
        await message.answer("Please send /start to register first!")
        return
    
    if _alarm_manager:
        if _alarm_manager.cancel_user_alarm(user.id):
            await message.answer("✅ Alarm cancelled!")
        else:
            await message.answer("ℹ️ No scheduled alarm to cancel.")


@router.message(Command("stats"))
async def cmd_stats(message: Message):
    """Handle /stats command."""
    chat_id = str(message.chat.id)
    user = _get_user(chat_id)
    
    if not user:
        await message.answer("Please send /start to register first!")
        return
    
    stats = db.get_user_stats(user.id)
    
    if stats["total_completed"] == 0:
        await message.answer(
            "📊 *No statistics yet*\n\n"
            "Complete your first wake-up to start tracking!",
            parse_mode=ParseMode.MARKDOWN
        )
    else:
        avg_time = stats["avg_wake_time_seconds"]
        minutes = int(avg_time // 60)
        seconds = int(avg_time % 60)
        
        await message.answer(
            f"📊 *Wake-Up Statistics*\n\n"
            f"Total wake-ups: {stats['total_completed']}\n"
            f"Average time: {minutes}m {seconds}s",
            parse_mode=ParseMode.MARKDOWN
        )


@router.message(Command("test"))
async def cmd_test(message: Message):
    """Handle /test command - trigger alarm for testing."""
    chat_id = str(message.chat.id)
    user = _get_user(chat_id)
    
    if not user:
        await message.answer("Please send /start to register first!")
        return
    
    if _alarm_manager:
        now = datetime.now()
        alarm = _alarm_manager.set_alarm(user.id, now.hour, now.minute)
        
        await message.answer(
            "🧪 *Test mode activated!*\n\n"
            "Alarm will trigger in a few seconds...",
            parse_mode=ParseMode.MARKDOWN
        )


@router.message(F.text)
async def handle_text(message: Message):
    """Handle text messages (for registration)."""
    chat_id = str(message.chat.id)
    state = _user_states.get(chat_id)
    
    if state == WAITING_FOR_NAME:
        name = message.text.strip()
        
        if len(name) < 1 or len(name) > 50:
            await message.answer("Please enter a valid name (1-50 characters).")
            return
        
        user = db.register_user(chat_id, name)
        _user_states.pop(chat_id, None)
        
        await message.answer(
            f"✅ *Registration complete!*\n\n"
            f"Nice to meet you, *{user.name}*! 🎉\n\n"
            "*Commands:*\n"
            "• `/set HH:MM` - Set alarm (e.g., /set 07:30)\n"
            "• `/status` - Check current alarm\n"
            "• `/cancel` - Cancel scheduled alarm\n"
            "• `/stats` - View wake-up statistics\n"
            "• `/test` - Test alarm immediately",
            parse_mode=ParseMode.MARKDOWN
        )
    else:
        user = _get_user(chat_id)
        if not user:
            await message.answer("👋 Please send /start to register first!")
        else:
            await message.answer("Use /help to see available commands.")


class TelegramBot:
    """Telegram bot wrapper using aiogram."""
    
    def __init__(self):
        self.bot: Optional[Bot] = None
        self.dp: Optional[Dispatcher] = None
        self._polling_task: Optional[asyncio.Task] = None
    
    def set_alarm_manager(self, manager):
        """Set alarm manager reference."""
        set_alarm_manager(manager)
    
    async def start(self):
        """Initialize and start the Telegram bot."""
        global _bot
        
        if not config.TELEGRAM_BOT_TOKEN:
            print("[BOT] ERROR: TELEGRAM_BOT_TOKEN not set in .env!")
            return
        
        print(f"[BOT] Initializing with token: {config.TELEGRAM_BOT_TOKEN[:10]}...{config.TELEGRAM_BOT_TOKEN[-5:]}")
        
        try:
            self.bot = Bot(token=config.TELEGRAM_BOT_TOKEN)
            _bot = self.bot
            
            # Test connection
            bot_info = await self.bot.get_me()
            print(f"[BOT] Connected as @{bot_info.username} ({bot_info.first_name})")
            
            # Create dispatcher
            self.dp = Dispatcher()
            self.dp.include_router(router)
            
            # Start polling in background
            self._polling_task = asyncio.create_task(self._run_polling())
            
            print(f"[BOT] Started successfully! Send /start to @{bot_info.username}")
            
        except Exception as e:
            print(f"[BOT] ERROR starting bot: {e}")
            import traceback
            traceback.print_exc()
            self.bot = None
    
    async def _run_polling(self):
        """Run polling in background."""
        try:
            await self.dp.start_polling(self.bot, drop_pending_updates=True)
        except asyncio.CancelledError:
            pass
        except Exception as e:
            print(f"[BOT] Polling error: {e}")
    
    async def stop(self):
        """Stop the Telegram bot."""
        if self._polling_task:
            self._polling_task.cancel()
            try:
                await self._polling_task
            except asyncio.CancelledError:
                pass
        
        if self.bot:
            await self.bot.session.close()
            print("[BOT] Stopped")
    
    async def send_alarm_qr(self, user: User, qr_image: bytes):
        """Send QR code to user when alarm triggers."""
        if not self.bot:
            return
        
        photo = BufferedInputFile(qr_image, filename="qr_code.png")
        
        await self.bot.send_photo(
            chat_id=user.chat_id,
            photo=photo,
            caption=(
                f"🔔 *ALARM!* 🔔\n\n"
                f"Time to wake up, {user.name}!\n"
                f"Scan this QR code with your camera to stop the alarm."
            ),
            parse_mode=ParseMode.MARKDOWN
        )
    
    async def send_alarm_stopped(self, user: User, alarm: Alarm):
        """Send message when alarm is successfully stopped."""
        if not self.bot:
            return
        
        wake_time = alarm.wake_time_seconds or 0
        minutes = wake_time // 60
        seconds = wake_time % 60
        
        if minutes > 0:
            time_str = f"{minutes}m {seconds}s"
        else:
            time_str = f"{seconds}s"
        
        await self.bot.send_message(
            chat_id=user.chat_id,
            text=(
                f"☀️ *Good morning, {user.name}!*\n\n"
                f"You woke up in: *{time_str}*\n"
                f"Keep it up! 💪"
            ),
            parse_mode=ParseMode.MARKDOWN
        )


# Singleton instance
telegram_bot = TelegramBot()
