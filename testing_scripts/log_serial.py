import serial


with serial.Serial("/dev/ttyACM0", 115200, timeout=1) as sr:
    try:
        while True:
            print(sr.readline().decode().strip())
    except:
        print("Exiting...")
