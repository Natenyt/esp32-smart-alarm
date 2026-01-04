"""
QR Code handler - generation and validation
"""
import io
import uuid
from typing import Optional

import qrcode
from PIL import Image
from pyzbar.pyzbar import decode as decode_qr


def generate_qr_token() -> str:
    """Generate a unique token for QR code."""
    return f"alarm_{uuid.uuid4().hex[:12]}"


def generate_qr_image(token: str) -> bytes:
    """
    Generate a QR code image containing the token.
    Returns PNG image as bytes.
    """
    qr = qrcode.QRCode(
        version=1,
        error_correction=qrcode.constants.ERROR_CORRECT_M,
        box_size=10,
        border=4,
    )
    qr.add_data(token)
    qr.make(fit=True)
    
    img = qr.make_image(fill_color="black", back_color="white")
    
    # Convert to bytes
    buffer = io.BytesIO()
    img.save(buffer, format="PNG")
    buffer.seek(0)
    
    return buffer.getvalue()


def decode_qr_from_jpeg(image_bytes: bytes) -> Optional[str]:
    """
    Decode QR code from JPEG image bytes.
    Returns the decoded string or None if no QR found.
    """
    try:
        # Open image from bytes
        image = Image.open(io.BytesIO(image_bytes))
        
        # Decode QR codes
        decoded_objects = decode_qr(image)
        
        if decoded_objects:
            # Return the first QR code found
            return decoded_objects[0].data.decode("utf-8")
        
        return None
    
    except Exception as e:
        print(f"QR decode error: {e}")
        return None


def validate_token(scanned: str, expected: str) -> bool:
    """Check if scanned token matches expected token."""
    return scanned.strip() == expected.strip()
