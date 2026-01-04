"""
Run just the Telegram bot (for testing separately)
"""
import asyncio
import sys
from telegram_bot import telegram_bot
from alarm_manager import alarm_manager
from config import config


async def main():
    print("=" * 50)
    print("Smart Alarm - Telegram Bot Only")
    print("=" * 50)
    
    if not config.TELEGRAM_BOT_TOKEN:
        print("ERROR: TELEGRAM_BOT_TOKEN not set in .env!")
        return
    
    print(f"Bot Token: {config.TELEGRAM_BOT_TOKEN[:10]}...{config.TELEGRAM_BOT_TOKEN[-5:]}")
    
    # Wire up components
    telegram_bot.set_alarm_manager(alarm_manager)
    alarm_manager.set_telegram_bot(telegram_bot)
    
    # Start components
    await alarm_manager.start()
    await telegram_bot.start()
    
    # Check if bot actually started
    if not telegram_bot.bot:
        print("\nERROR: Bot failed to start!")
        await alarm_manager.stop()
        sys.exit(1)
    
    print("=" * 50)
    print("Bot is running! Press Ctrl+C to stop.")
    print("=" * 50)
    
    # Keep running
    try:
        while True:
            await asyncio.sleep(1)
    except KeyboardInterrupt:
        print("\nStopping...")
    finally:
        await telegram_bot.stop()
        await alarm_manager.stop()


if __name__ == "__main__":
    asyncio.run(main())
