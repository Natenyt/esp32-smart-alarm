"""
Configuration module - loads settings from .env file
"""
import os
from pathlib import Path
from dotenv import load_dotenv

# Load .env from project root
env_path = Path(__file__).parent.parent / ".env"
load_dotenv(env_path)


class Config:
    """Application configuration loaded from environment variables."""
    
    # Telegram
    TELEGRAM_BOT_TOKEN: str = os.getenv("TELEGRAM_BOT_TOKEN", "")
    
    # Server
    SERVER_HOST: str = os.getenv("SERVER_HOST", "0.0.0.0")
    SERVER_PORT: int = int(os.getenv("SERVER_PORT", "8000"))
    
    # Ngrok
    USE_NGROK: bool = os.getenv("USE_NGROK", "false").lower() == "true"
    NGROK_AUTHTOKEN: str = os.getenv("NGROK_AUTHTOKEN", "")
    
    # Database
    DATABASE_PATH: Path = Path(__file__).parent / "alarm_data.db"
    
    @classmethod
    def validate(cls) -> list[str]:
        """Validate required configuration. Returns list of missing items."""
        missing = []
        if not cls.TELEGRAM_BOT_TOKEN:
            missing.append("TELEGRAM_BOT_TOKEN")
        return missing


config = Config()
