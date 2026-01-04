"""
Run both server and bot together
"""
import asyncio
import uvicorn
from contextlib import asynccontextmanager

from fastapi import FastAPI, Request, HTTPException

from config import config
from alarm_manager import alarm_manager
from telegram_bot import telegram_bot
from qr_handler import decode_qr_from_jpeg


@asynccontextmanager
async def lifespan(app: FastAPI):
    print("=" * 50)
    print("Smart Alarm - Full Server")
    print("=" * 50)
    
    # Wire up
    telegram_bot.set_alarm_manager(alarm_manager)
    alarm_manager.set_telegram_bot(telegram_bot)
    
    # Start
    await alarm_manager.start()
    await telegram_bot.start()
    
    print(f"Server: http://{config.SERVER_HOST}:{config.SERVER_PORT}")
    print("=" * 50)
    
    yield
    
    await telegram_bot.stop()
    await alarm_manager.stop()


app = FastAPI(title="Smart Alarm", lifespan=lifespan)


@app.get("/api/device/poll")
async def device_poll():
    return {"state": alarm_manager.get_device_state()}


@app.post("/api/device/scan")
async def device_scan(request: Request):
    image_bytes = await request.body()
    if not image_bytes:
        raise HTTPException(status_code=400, detail="No image")
    
    decoded = decode_qr_from_jpeg(image_bytes)
    if not decoded:
        return {"action": "CONTINUE"}
    
    return await alarm_manager.process_scan(decoded)


@app.get("/")
async def root():
    return {"status": "ok"}


if __name__ == "__main__":
    uvicorn.run("main:app", host=config.SERVER_HOST, port=config.SERVER_PORT, reload=True)
