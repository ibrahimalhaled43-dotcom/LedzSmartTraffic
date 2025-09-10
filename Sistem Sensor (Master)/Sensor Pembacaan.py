import cv2
import math
import cvzone
import numpy as np
from sort import Sort
from ultralytics import YOLO
import pandas as pd
from datetime import datetime, timedelta
import os
import requests
import json

# ========== Konfigurasi ==========
ESP32_URL = "http://192.168.100.3/update"  # Sesuaikan dengan IP ESP32
CONFIDENCE_THRESHOLD = 0.3
YOLO_MODEL = "best.pt"

# Garis counting berbasis rasio
UP_LINE_RATIO = 0.1  # 40% dari tinggi frame
DOWN_LINE_RATIO = 0.15 # 60% dari tinggi frame

# ========== Inisialisasi ==========
print("🚀 Initializing Vehicle Counter...")
print("Loading YOLO model...")
try:
    model = YOLO(YOLO_MODEL)
    print("✓ YOLO model loaded successfully")
except Exception as e:
    print(f"✗ Error loading YOLO model: {e}")
    print("Make sure yolov8n.pt is downloaded or available")
    exit(1)

print("Connecting to camera stream...")
cap = cv2.VideoCapture("http://192.168.100.2:81/stream")

# Cek apakah camera berhasil terbuka
if not cap.isOpened():
    print("✗ Failed to open camera stream")
    print("Trying alternative: webcam (0)")
    cap = cv2.VideoCapture(0)
    if not cap.isOpened():
        print("✗ No camera available")
        exit(1)

print("✓ Camera connected")

# Load mask (optional)
mask = None
if os.path.exists("Media\mask.png"):
    mask = cv2.imread("Media\mask.png", cv2.IMREAD_GRAYSCALE)
    print("✓ Mask loaded")
else:
    print("⚠ mask.png not found, running without mask")

# Initialize tracker
try:
    tracker = Sort(max_age=20, min_hits=3, iou_threshold=0.3)
    print("✓ SORT tracker initialized")
except Exception as e:
    print(f"✗ Error initializing tracker: {e}")
    print("Make sure sort.py is available")
    exit(1)

up_count, down_count = 0, 0
up_ids, down_ids = set(), set()

log_data = []
start_time = datetime.now()
frame_count = 0

# ========== Fungsi bantu ==========
def send_to_esp32(naik, turun):
    """Kirim data counter ke ESP32 dengan format JSON yang benar"""
    try:
        # Siapkan data JSON
        data = {
            "count_up": naik,
            "count_down": turun,
            "timestamp": datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        }
        
        # Kirim POST request dengan JSON
        headers = {'Content-Type': 'application/json'}
        response = requests.post(ESP32_URL, 
                               data=json.dumps(data), 
                               headers=headers, 
                               timeout=1.0)
        
        if response.status_code == 200:
            print(f"📡 Data sent to ESP32: UP={naik}, DOWN={turun}")
            return True
        else:
            print(f"⚠ ESP32 response error: {response.status_code}")
            return False
            
    except requests.exceptions.Timeout:
        print("⚠ ESP32 timeout")
        return False
    except requests.exceptions.ConnectionError:
        print("⚠ ESP32 connection failed")
        return False
    except Exception as e:
        print(f"⚠ ESP32 error: {e}")
        return False

def save_to_excel(log_data):
    """Simpan data log ke Excel"""
    if not log_data:
        print("⚠ No data to save")
        return
        
    try:
        df = pd.DataFrame(log_data)
        
        # Buat Excel writer
        with pd.ExcelWriter("hasil_ringkasan.xlsx", engine="xlsxwriter") as writer:
            # Tulis semua data ke sheet "Rekap"
            df.to_excel(writer, sheet_name="Rekap", index=False)
            
            # Format worksheet
            workbook = writer.book
            worksheet = writer.sheets['Rekap']
            
            # Format header
            header_format = workbook.add_format({
                'bold': True,
                'text_wrap': True,
                'valign': 'top',
                'fg_color': '#D7E4BC',
                'border': 1
            })
            
            # Apply header format
            for col_num, value in enumerate(df.columns.values):
                worksheet.write(0, col_num, value, header_format)
            
            # Auto-adjust column width
            for i, col in enumerate(df.columns):
                column_len = max(df[col].astype(str).str.len().max(), len(col)) + 2
                worksheet.set_column(i, i, column_len)
        
        print("✓ Data saved to hasil_ringkasan.xlsx")
        
    except Exception as e:
        print(f"✗ Error saving Excel: {e}")

def test_esp32_connection():
    """Test koneksi ke ESP32"""
    try:
        response = requests.get("http://192.168.100.3/status", timeout=3.0)
        if response.status_code == 200:
            data = response.json()
            print("✓ ESP32 connection OK")
            print(f"  - IP: {data.get('ip_address', 'Unknown')}")
            print(f"  - LCD: {'Connected' if data.get('lcd_connected') else 'Disconnected'}")
            return True
    except requests.exceptions.ConnectionError:
        print("✗ ESP32 not reachable - Check IP address and network")
    except requests.exceptions.Timeout:
        print("✗ ESP32 connection timeout")
    except Exception as e:
        print(f"✗ ESP32 test error: {e}")
    
    return False

def get_class_name(class_id):
    """Dapatkan nama kelas berdasarkan ID COCO"""
    coco_names = {
        0: 'person', 1: 'bicycle', 2: 'car', 3: 'motorcycle', 4: 'airplane',
        5: 'bus', 6: 'train', 7: 'truck', 8: 'boat', 9: 'traffic light'
    }
    return coco_names.get(class_id, f'class_{class_id}')

# ========== Test koneksi ESP32 di awal ==========
print("\n🔌 Testing ESP32 connection...")
esp32_available = test_esp32_connection()

if not esp32_available:
    print("⚠ Continuing without ESP32 connection")
    print("Data will be logged locally only")

print("\n🎬 Starting vehicle detection...")
print("Press 'q' to quit, 'r' to reset counters")

# ========== Loop utama ==========
try:
    while True:
        success, frame = cap.read()
        if not success:
            print("✗ Failed to read frame - retrying...")
            # Coba reconnect ke stream
            cap.release()
            cap = cv2.VideoCapture("http://192.168.100.2:81/stream")
            if not cap.isOpened():
                print("✗ Cannot reconnect to camera")
                break
            continue

        frame_count += 1
        h, w, _ = frame.shape

        # Resize dan masking
        # Resize dan masking semi-transparan
        if mask is not None:
            try:
                mask_resized = cv2.resize(mask, (w, h))
                mask_binary = cv2.threshold(mask_resized, 127, 255, cv2.THRESH_BINARY)[1]
                mask_colored = cv2.merge([mask_binary]*3)

                # Warna overlay untuk mask (contoh: hijau)
                overlay_color = np.zeros_like(frame)
                overlay_color[:] = (0, 255, 0)  # Hijau terang

                # Apply warna hanya pada area yang dimask
                mask_area = cv2.bitwise_and(overlay_color, mask_colored)

                # Gabungkan dengan frame asli secara transparan
                alpha = 0.7  # Semakin rendah, semakin transparan
                masked_frame = cv2.addWeighted(frame, 1.0, mask_area, alpha, 0)

            except Exception as e:
                print(f"⚠ Mask error: {e} - using original frame")
                masked_frame = frame
        else:
            masked_frame = frame

        # YOLO detection
        try:
            results = model(masked_frame, stream=True, conf=CONFIDENCE_THRESHOLD, verbose=False)
        except Exception as e:
            print(f"⚠ YOLO detection error: {e}")
            continue

        detections = []
        for r in results:
            if r.boxes is not None:
                for box, conf, cls in zip(r.boxes.xyxy, r.boxes.conf, r.boxes.cls):
                    x1, y1, x2, y2 = map(int, box)
                    class_id = int(cls)
                    confidence = float(conf)
                    
                    # Deteksi kendaraan: person(0), car(2), motorcycle(3), bus(5), truck(7)
                    if class_id in [0, 2, 3, 5, 7] and confidence > CONFIDENCE_THRESHOLD:
                        detections.append([x1, y1, x2, y2, confidence])

        # Tracking
        try:
            if len(detections) > 0:
                track_results = tracker.update(np.array(detections))
            else:
                track_results = tracker.update(np.empty((0, 5)))
        except Exception as e:
            print(f"⚠ Tracking error: {e}")
            track_results = np.empty((0, 5))

        up_line = int(h * UP_LINE_RATIO)
        down_line = int(h * DOWN_LINE_RATIO)

        # Tracking dan counting
        for track in track_results:
            x1, y1, x2, y2, track_id = track.astype(int)
            cx, cy = (x1 + x2) // 2, (y1 + y2) // 2

            # Hitung crossing
            if track_id not in up_ids and cy < up_line:
                up_ids.add(track_id)
                up_count += 1
                print(f"🔺 Vehicle UP detected! ID: {track_id}, Total UP: {up_count}")
                
                # Kirim ke ESP32 setiap ada perubahan
                if esp32_available:
                    send_to_esp32(up_count, down_count)

            elif track_id not in down_ids and cy > down_line:
                down_ids.add(track_id)
                down_count += 1
                print(f"🔻 Vehicle DOWN detected! ID: {track_id}, Total DOWN: {down_count}")
                
                # Kirim ke ESP32 setiap ada perubahan
                if esp32_available:
                    send_to_esp32(up_count, down_count)

            # Tampilkan tracking
            try:
                cvzone.putTextRect(frame, f'ID:{track_id}', (x1, y1-10), 1, 1, 
                                 colorR=(255, 255, 255), colorT=(0, 0, 0))
                cv2.rectangle(frame, (x1, y1), (x2, y2), (255, 0, 255), 2)
                cv2.circle(frame, (cx, cy), 4, (255, 255, 255), -1)
            except Exception as e:
                # Fallback jika cvzone error
                cv2.rectangle(frame, (x1, y1), (x2, y2), (255, 0, 255), 2)
                cv2.putText(frame, f'ID:{track_id}', (x1, y1-10), 
                          cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 2)

        # Gambar garis counting
        cv2.line(frame, (0, up_line), (w, up_line), (255, 0, 255), 3)
        cv2.line(frame, (0, down_line), (w, down_line), (0, 255, 255), 3)
        
        # Label garis
        cv2.putText(frame, "UP LINE", (10, up_line-10), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 0, 255), 2)
        cv2.putText(frame, "DOWN LINE", (10, down_line+25), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 255), 2)
        
        # Tampilkan counter
        try:
            cvzone.putTextRect(frame, f'UP: {up_count}', (10, 50), 2, 2, 
                             colorR=(0, 255, 0), colorT=(0, 0, 0))
            cvzone.putTextRect(frame, f'DOWN: {down_count}', (10, 100), 2, 2, 
                             colorR=(0, 0, 255), colorT=(255, 255, 255))
        except:
            # Fallback tanpa cvzone
            cv2.putText(frame, f'UP: {up_count}', (10, 50), cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 0), 2)
            cv2.putText(frame, f'DOWN: {down_count}', (10, 100), cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 0, 255), 2)

        # Info tambahan
        current_time = datetime.now().strftime("%H:%M:%S")
        cv2.putText(frame, f'Time: {current_time}', (w-200, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 2)
        cv2.putText(frame, f'FPS: {frame_count/max((datetime.now() - start_time).total_seconds(), 1):.1f}', 
                   (w-200, 60), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 2)

        # Hitung waktu dan simpan log per menit
        now = datetime.now()
        elapsed = (now - start_time).total_seconds()
        
        if elapsed >= 60:  # Setiap 1 menit
            fps = frame_count / elapsed
            img_size_kb = (w * h * 3 / 1024) * frame_count  # estimasi size KB

            log_data.append({
                "Waktu Mulai": start_time.strftime("%H:%M:%S"),
                "Waktu Selesai": now.strftime("%H:%M:%S"),
                "Durasi": f"{int(elapsed)} detik",
                "Resolusi": f"{w}x{h}",
                "FPS": round(fps, 2),
                "Naik": up_count,
                "Turun": down_count,
                "Total": up_count + down_count,
                "Ukuran Gambar / Menit (KB)": int(img_size_kb)
            })

            print(f"📊 Log saved: UP={up_count}, DOWN={down_count}, Total={up_count+down_count}, FPS={fps:.2f}")
            
            # Reset untuk periode berikutnya (tapi tidak reset counter total)
            start_time = now
            frame_count = 0
            up_ids.clear()
            down_ids.clear()

        # Tampilkan frame
        cv2.imshow("Vehicle Counter - Press 'q' to quit, 'r' to reset", frame)
        
        # Handle keyboard input
        key = cv2.waitKey(1) & 0xFF
        if key == ord('q'):
            print("🛑 Quit requested by user")
            break
        elif key == ord('r'):
            print("🔄 Resetting counters...")
            up_count = down_count = 0
            up_ids.clear()
            down_ids.clear()
            if esp32_available:
                send_to_esp32(0, 0)

except KeyboardInterrupt:
    print("\n🛑 Interrupted by user (Ctrl+C)")
except Exception as e:
    print(f"\n✗ Unexpected error: {e}")
    import traceback
    traceback.print_exc()

finally:
    # ========== Cleanup ==========
    print("\n🧹 Cleaning up...")
    
    # Simpan sisa log
    if frame_count > 0:
        now = datetime.now()
        elapsed_final = (now - start_time).total_seconds()
        if elapsed_final > 0:
            fps = frame_count / elapsed_final
            img_size_kb = (w * h * 3 / 1024) * frame_count
            log_data.append({
                "Waktu Mulai": start_time.strftime("%H:%M:%S"),
                "Waktu Selesai": now.strftime("%H:%M:%S"),
                "Durasi": f"{int(elapsed_final)} detik",
                "Resolusi": f"{w}x{h}",
                "FPS": round(fps, 2),
                "Naik": up_count,
                "Turun": down_count,
                "Total": up_count + down_count,
                "Ukuran Gambar / Menit (KB)": int(img_size_kb)
            })

    # Kirim data final ke ESP32
    if esp32_available:
        print("📡 Sending final data to ESP32...")
        send_to_esp32(up_count, down_count)

    # Simpan Excel
    if log_data:
        print("💾 Saving data to Excel...")
        save_to_excel(log_data)

    print(f"🏁 Final count - UP: {up_count}, DOWN: {down_count}, Total: {up_count + down_count}")
    
    # Release resources
    cap.release()
    cv2.destroyAllWindows()
    print("✓ Resources released")
    print("👋 Program ended successfully")