import os
import pandas as pd
import numpy as np

CSV_FILENAME = "swap_rf_data.csv"

def generate_dummy_data():
    """Generates a synthetic dataset for testing the Random Forest pipeline."""
    print(f"Generating synthetic dataset of 600 samples for SWAP project...")
    np.random.seed(42)
    
    # Target Classes: 0 = Wi-Fi, 1 = BLE, 2 = LoRa
    
    # Generate Wi-Fi optimal scenarios (Strong Wi-Fi, low loss)
    wifi_data = {
        "wifi_rssi": np.random.randint(-70, -40, 200),
        "ble_rssi": np.random.randint(-100, -60, 200),
        "lora_rssi": np.random.randint(-120, -90, 200),
        "lora_snr": np.random.uniform(-5.0, 5.0, 200),
        "pdr": np.random.uniform(0.9, 1.0, 200),
        "rtt": np.random.uniform(10.0, 50.0, 200),
        "retries": np.random.randint(0, 2, 200),
        "best_protocol": [0] * 200
    }
    
    # Generate BLE optimal scenarios (Weak Wi-Fi, strong BLE)
    ble_data = {
        "wifi_rssi": np.random.randint(-95, -75, 200),
        "ble_rssi": np.random.randint(-75, -50, 200),
        "lora_rssi": np.random.randint(-120, -90, 200),
        "lora_snr": np.random.uniform(-5.0, 5.0, 200),
        "pdr": np.random.uniform(0.8, 1.0, 200),
        "rtt": np.random.uniform(20.0, 100.0, 200),
        "retries": np.random.randint(0, 3, 200),
        "best_protocol": [1] * 200
    }
    
    # Generate LoRa optimal scenarios (Weak Wi-Fi & BLE, high loss/RTT)
    lora_data = {
        "wifi_rssi": np.random.randint(-100, -85, 200),
        "ble_rssi": np.random.randint(-100, -85, 200),
        "lora_rssi": np.random.randint(-110, -70, 200),
        "lora_snr": np.random.uniform(0.0, 12.0, 200),
        "pdr": np.random.uniform(0.4, 0.8, 200),
        "rtt": np.random.uniform(100.0, 1000.0, 200),
        "retries": np.random.randint(2, 6, 200),
        "best_protocol": [2] * 200
    }
    
    # Combine and shuffle
    df_wifi = pd.DataFrame(wifi_data)
    df_ble = pd.DataFrame(ble_data)
    df_lora = pd.DataFrame(lora_data)
    
    df = pd.concat([df_wifi, df_ble, df_lora]).sample(frac=1).reset_index(drop=True)
    
    df.to_csv(CSV_FILENAME, index=False)
    print(f"Created {CSV_FILENAME} with {len(df)} samples.")
    print("Class distribution:")
    print(df['best_protocol'].value_counts())

if __name__ == "__main__":
    generate_dummy_data()