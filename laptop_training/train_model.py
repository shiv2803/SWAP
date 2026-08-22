import pandas as pd
from sklearn.ensemble import RandomForestClassifier
from sklearn.model_selection import train_test_split
from sklearn.metrics import classification_report, confusion_matrix
import joblib

CSV_FILENAME = "swap_rf_data.csv"
MODEL_FILENAME = "swap_random_forest.joblib"
FEATURE_SEQUENCE_FILE = "feature_sequence.txt"

# Exact sequence expected by the UNO Q inference engine
FEATURE_COLS = [
    "wifi_rssi", "ble_rssi", "lora_rssi", "lora_snr",
    "pdr", "rtt", "retries"
]
TARGET_COL = "best_protocol"

def main():
    print(f"Loading dataset from {CSV_FILENAME}...")
    try:
        df = pd.read_csv(CSV_FILENAME)
    except FileNotFoundError:
        print(f"Error: {CSV_FILENAME} not found. Ensure you ran the dummy data generator.")
        return

    # Enforce strict feature selection
    X = df[FEATURE_COLS]
    y = df[TARGET_COL]

    print("Splitting data (80% train, 20% test, stratified)...")
    X_train, X_test, y_train, y_test = train_test_split(
        X, y, test_size=0.20, random_state=42, stratify=y
    )

    print("Training Random Forest (100 estimators, max_depth=8)...")
    rf = RandomForestClassifier(
        n_estimators=100,
        max_depth=8,
        min_samples_leaf=5,
        class_weight="balanced",
        random_state=42,
        n_jobs=-1
    )
    rf.fit(X_train, y_train)

    print("\n--- Model Evaluation ---")
    y_pred = rf.predict(X_test)
    print("Confusion Matrix:")
    print(confusion_matrix(y_test, y_pred))
    print("\nClassification Report:")
    print(classification_report(y_test, y_pred, target_names=["Wi-Fi (0)", "BLE (1)", "LoRa (2)"]))

    print(f"Saving model to {MODEL_FILENAME}...")
    joblib.dump(rf, MODEL_FILENAME)
    
    with open(FEATURE_SEQUENCE_FILE, "w") as f:
        f.write(",".join(FEATURE_COLS))
        
    print(f"Done. Model saved as {MODEL_FILENAME}. Feature order documented in {FEATURE_SEQUENCE_FILE}.")

if __name__ == "__main__":
    main()