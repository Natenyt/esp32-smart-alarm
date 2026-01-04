"""
Run just the FastAPI server (for ESP32 communication)
"""
import uvicorn
from fastapi import FastAPI, Request, HTTPException
from contextlib import asynccontextmanager

from config import config
from alarm_manager import alarm_manager
from qr_handler import decode_qr_from_jpeg


@asynccontextmanager
async def lifespan(app: FastAPI):
    print("=" * 50)
    print("Smart Alarm - API Server")
    print("=" * 50)
    
    await alarm_manager.start()
    
    print(f"Server: http://{config.SERVER_HOST}:{config.SERVER_PORT}")
    print("=" * 50)
    
    yield
    
    await alarm_manager.stop()


app = FastAPI(title="Smart Alarm API", lifespan=lifespan)


@app.get("/api/device/poll")
async def device_poll():
    state = alarm_manager.get_device_state()
    print(f"[POLL] State: {state}")
    return {"state": state}


@app.post("/api/device/scan")
async def device_scan(request: Request):
    image_bytes = await request.body()
    
    if not image_bytes:
        raise HTTPException(status_code=400, detail="No image")
    
    print(f"[SCAN] Received {len(image_bytes)} bytes")
    
    decoded = decode_qr_from_jpeg(image_bytes)
    
    if not decoded:
        return {"action": "CONTINUE", "message": "No QR found"}
    
    print(f"[SCAN] QR: {decoded}")
    result = await alarm_manager.process_scan(decoded)
    return result


@app.get("/")
async def root():
    return {"status": "running", "device_state": alarm_manager.get_device_state()}


if __name__ == "__main__":
    uvicorn.run("run_server:app", host=config.SERVER_HOST, port=config.SERVER_PORT, reload=True)
