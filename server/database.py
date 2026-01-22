"""
Database module - SQLite setup and models for alarm persistence
"""
import sqlite3
from datetime import datetime
from pathlib import Path
from typing import Optional
from dataclasses import dataclass
from enum import Enum

from config import config


class AlarmState(Enum):
    """Possible states for an alarm."""
    SCHEDULED = "scheduled"      # Waiting for trigger time
    RINGING = "ringing"          # Currently ringing, waiting for QR scan
    COMPLETED = "completed"      # Successfully stopped by QR scan
    EXPIRED = "expired"          # Timed out without being stopped
    CANCELLED = "cancelled"      # Manually cancelled by user


@dataclass
class User:
    """Registered user model."""
    id: int
    chat_id: str                 # Telegram chat ID
    name: str                    # User's name
    registered_at: datetime


@dataclass
class Alarm:
    """Alarm data model."""
    id: int
    user_id: int                 # Foreign key to user
    trigger_time: datetime       # When the alarm should ring
    state: AlarmState
    qr_token: Optional[str]      # Unique token for this alarm session
    created_at: datetime
    triggered_at: Optional[datetime]   # When alarm started ringing
    stopped_at: Optional[datetime]     # When alarm was stopped
    wake_time_seconds: Optional[int]   # How long it took to wake up
    notification_sent: bool = False    # Whether success message was sent


class Database:
    """SQLite database handler for alarm persistence."""
    
    def __init__(self, db_path: Path = config.DATABASE_PATH):
        self.db_path = db_path
        self._init_db()
    
    def _get_connection(self) -> sqlite3.Connection:
        """Get a database connection with row factory."""
        conn = sqlite3.connect(self.db_path)
        conn.row_factory = sqlite3.Row
        return conn
    
    def _init_db(self):
        """Initialize database schema."""
        conn = self._get_connection()
        cursor = conn.cursor()
        
        # Users table
        cursor.execute("""
            CREATE TABLE IF NOT EXISTS users (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                chat_id TEXT UNIQUE NOT NULL,
                name TEXT NOT NULL,
                registered_at TEXT NOT NULL
            )
        """)
        
        # Alarms table with user foreign key
        cursor.execute("""
            CREATE TABLE IF NOT EXISTS alarms (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                user_id INTEGER NOT NULL,
                trigger_time TEXT NOT NULL,
                state TEXT NOT NULL DEFAULT 'scheduled',
                qr_token TEXT,
                created_at TEXT NOT NULL,
                triggered_at TEXT,
                stopped_at TEXT,
                wake_time_seconds INTEGER,
                notification_sent INTEGER DEFAULT 0,
                qr_sent INTEGER DEFAULT 0,
                FOREIGN KEY (user_id) REFERENCES users(id)
            )
        """)
        
        # Index for quick lookup of active alarms
        cursor.execute("""
            CREATE INDEX IF NOT EXISTS idx_alarm_state 
            ON alarms(state)
        """)

        
        # Index for user lookup by chat_id
        cursor.execute("""
            CREATE INDEX IF NOT EXISTS idx_user_chat_id 
            ON users(chat_id)
        """)
        
        conn.commit()
        conn.close()
    
    # ===================
    # USER METHODS
    # ===================
    
    def _row_to_user(self, row: sqlite3.Row) -> User:
        """Convert database row to User object."""
        return User(
            id=row["id"],
            chat_id=row["chat_id"],
            name=row["name"],
            registered_at=datetime.fromisoformat(row["registered_at"])
        )
    
    def get_user_by_chat_id(self, chat_id: str) -> Optional[User]:
        """Get user by Telegram chat ID."""
        conn = self._get_connection()
        cursor = conn.cursor()
        
        cursor.execute("SELECT * FROM users WHERE chat_id = ?", (str(chat_id),))
        row = cursor.fetchone()
        conn.close()
        
        return self._row_to_user(row) if row else None
    
    def get_user_by_id(self, user_id: int) -> Optional[User]:
        """Get user by ID."""
        conn = self._get_connection()
        cursor = conn.cursor()
        
        cursor.execute("SELECT * FROM users WHERE id = ?", (user_id,))
        row = cursor.fetchone()
        conn.close()
        
        return self._row_to_user(row) if row else None
    
    def register_user(self, chat_id: str, name: str) -> User:
        """Register a new user."""
        conn = self._get_connection()
        cursor = conn.cursor()
        
        now = datetime.now()
        cursor.execute("""
            INSERT INTO users (chat_id, name, registered_at)
            VALUES (?, ?, ?)
        """, (str(chat_id), name, now.isoformat()))
        
        user_id = cursor.lastrowid
        conn.commit()
        conn.close()
        
        return User(
            id=user_id,
            chat_id=str(chat_id),
            name=name,
            registered_at=now
        )
    
    def get_all_users(self) -> list[User]:
        """Get all registered users."""
        conn = self._get_connection()
        cursor = conn.cursor()
        
        cursor.execute("SELECT * FROM users")
        rows = cursor.fetchall()
        conn.close()
        
        return [self._row_to_user(row) for row in rows]
    
    # ===================
    # ALARM METHODS
    # ===================
    
    def _row_to_alarm(self, row: sqlite3.Row) -> Alarm:
        """Convert database row to Alarm object."""
        return Alarm(
            id=row["id"],
            user_id=row["user_id"],
            trigger_time=datetime.fromisoformat(row["trigger_time"]),
            state=AlarmState(row["state"]),
            qr_token=row["qr_token"],
            created_at=datetime.fromisoformat(row["created_at"]),
            triggered_at=datetime.fromisoformat(row["triggered_at"]) if row["triggered_at"] else None,
            stopped_at=datetime.fromisoformat(row["stopped_at"]) if row["stopped_at"] else None,
            wake_time_seconds=row["wake_time_seconds"],
            # Defaults to False for backward compatibility or new rows
            notification_sent=bool(row["notification_sent"]) if "notification_sent" in row.keys() else False
        )
    
    def create_alarm(self, user_id: int, trigger_time: datetime) -> Alarm:
        """Create a new scheduled alarm for a user."""
        conn = self._get_connection()
        cursor = conn.cursor()
        
        now = datetime.now()
        cursor.execute("""
            INSERT INTO alarms (user_id, trigger_time, state, created_at)
            VALUES (?, ?, ?, ?)
        """, (user_id, trigger_time.isoformat(), AlarmState.SCHEDULED.value, now.isoformat()))
        
        alarm_id = cursor.lastrowid
        conn.commit()
        conn.close()
        
        return Alarm(
            id=alarm_id,
            user_id=user_id,
            trigger_time=trigger_time,
            state=AlarmState.SCHEDULED,
            qr_token=None,
            created_at=now,
            triggered_at=None,
            stopped_at=None,
            wake_time_seconds=None
        )
    
    def get_alarm(self, alarm_id: int) -> Optional[Alarm]:
        """Get alarm by ID."""
        conn = self._get_connection()
        cursor = conn.cursor()
        
        cursor.execute("SELECT * FROM alarms WHERE id = ?", (alarm_id,))
        row = cursor.fetchone()
        conn.close()
        
        return self._row_to_alarm(row) if row else None
    
    def get_scheduled_alarms(self) -> list[Alarm]:
        """Get all scheduled (pending) alarms."""
        conn = self._get_connection()
        cursor = conn.cursor()
        
        cursor.execute("""
            SELECT * FROM alarms 
            WHERE state = ? 
            ORDER BY trigger_time ASC
        """, (AlarmState.SCHEDULED.value,))
        
        rows = cursor.fetchall()
        conn.close()
        
        return [self._row_to_alarm(row) for row in rows]
    
    def get_ringing_alarm(self) -> Optional[Alarm]:
        """Get the currently ringing alarm (should only be one)."""
        conn = self._get_connection()
        cursor = conn.cursor()
        
        cursor.execute("""
            SELECT * FROM alarms 
            WHERE state = ? 
            LIMIT 1
        """, (AlarmState.RINGING.value,))
        
        row = cursor.fetchone()
        conn.close()
        
        return self._row_to_alarm(row) if row else None
    
    def get_user_scheduled_alarms(self, user_id: int) -> list[Alarm]:
        """Get scheduled alarms for a specific user."""
        conn = self._get_connection()
        cursor = conn.cursor()
        
        cursor.execute("""
            SELECT * FROM alarms 
            WHERE user_id = ? AND state = ? 
            ORDER BY trigger_time ASC
        """, (user_id, AlarmState.SCHEDULED.value))
        
        rows = cursor.fetchall()
        conn.close()
        
        return [self._row_to_alarm(row) for row in rows]
    
    def update_alarm_state(
        self, 
        alarm_id: int, 
        state: AlarmState,
        qr_token: Optional[str] = None,
        triggered_at: Optional[datetime] = None,
        stopped_at: Optional[datetime] = None,
        wake_time_seconds: Optional[int] = None,
        notification_sent: Optional[bool] = None
    ):
        """Update alarm state and related fields."""
        conn = self._get_connection()
        cursor = conn.cursor()
        
        updates = ["state = ?"]
        values = [state.value]
        
        if qr_token is not None:
            updates.append("qr_token = ?")
            values.append(qr_token)
        
        if triggered_at is not None:
            updates.append("triggered_at = ?")
            values.append(triggered_at.isoformat())
        
        if stopped_at is not None:
            updates.append("stopped_at = ?")
            values.append(stopped_at.isoformat())
        
        if wake_time_seconds is not None:
            updates.append("wake_time_seconds = ?")
            values.append(wake_time_seconds)
            
        if notification_sent is not None:
            updates.append("notification_sent = ?")
            values.append(1 if notification_sent else 0)
        
        values.append(alarm_id)
        
        cursor.execute(f"""
            UPDATE alarms 
            SET {', '.join(updates)}
            WHERE id = ?
        """, values)
        
        conn.commit()
        conn.close()
    
    def cancel_user_scheduled_alarms(self, user_id: int) -> int:
        """Cancel all scheduled alarms for a user. Returns count of cancelled."""
        conn = self._get_connection()
        cursor = conn.cursor()
        
        cursor.execute("""
            UPDATE alarms 
            SET state = ? 
            WHERE user_id = ? AND state = ?
        """, (AlarmState.CANCELLED.value, user_id, AlarmState.SCHEDULED.value))
        
        count = cursor.rowcount
        conn.commit()
        conn.close()
        
        return count
    
    def get_user_stats(self, user_id: int) -> dict:
        """Get wake-up statistics for a user."""
        conn = self._get_connection()
        cursor = conn.cursor()
        
        cursor.execute("""
            SELECT COUNT(*) as count, AVG(wake_time_seconds) as avg_time
            FROM alarms 
            WHERE user_id = ? AND state = ? AND wake_time_seconds IS NOT NULL
        """, (user_id, AlarmState.COMPLETED.value))
        
        row = cursor.fetchone()
        conn.close()
        
        return {
            "total_completed": row["count"] or 0,
            "avg_wake_time_seconds": round(row["avg_time"] or 0, 1)
        }

    def get_unnotified_alarms(self) -> list[Alarm]:
        """Get completed alarms that haven't been notified yet."""
        conn = self._get_connection()
        cursor = conn.cursor()
        
        # Check if column exists first (migration safety)
        try:
            cursor.execute("""
                SELECT * FROM alarms 
                WHERE state = ? AND notification_sent = 0
            """, (AlarmState.COMPLETED.value,))
            rows = cursor.fetchall()
        except sqlite3.OperationalError:
            # Column might not exist yet if DB wasn't recreated
            rows = []
        
        conn.close()
        
        return [self._row_to_alarm(row) for row in rows]

    def mark_notification_sent(self, alarm_id: int):
        """Mark an alarm as notified."""
        self.update_alarm_state(alarm_id, AlarmState.COMPLETED, notification_sent=True)

    def get_ringing_alarms_needing_qr(self) -> list[Alarm]:
        """Get ringing alarms that haven't had QR code sent yet."""
        conn = self._get_connection()
        cursor = conn.cursor()
        
        try:
            cursor.execute("""
                SELECT * FROM alarms 
                WHERE state = ? AND qr_sent = 0
            """, (AlarmState.RINGING.value,))
            rows = cursor.fetchall()
        except sqlite3.OperationalError:
            # Column might not exist yet
            rows = []
        
        conn.close()
        
        return [self._row_to_alarm(row) for row in rows]

    def mark_qr_sent(self, alarm_id: int):
        """Mark that QR code was sent for this alarm."""
        conn = self._get_connection()
        cursor = conn.cursor()
        
        try:
            cursor.execute("""
                UPDATE alarms SET qr_sent = 1 WHERE id = ?
            """, (alarm_id,))
            conn.commit()
        except sqlite3.OperationalError:
            pass  # Column might not exist
        
        conn.close()


# Singleton instance
db = Database()
