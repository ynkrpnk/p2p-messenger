import pandas as pd
from sklearn.ensemble import RandomForestClassifier
from sklearn.metrics import classification_report, confusion_matrix

# ==========================================
# Завантаження обох наборів
# ==========================================
print("--- ЗАВАНТАЖЕННЯ ДАНИХ ---")
own = pd.read_csv('/home/yan/wifi_dataset.csv')
awid = pd.read_csv('/home/yan/awid_dataset.csv')

print("Власний набір, баланс класів:")
print(own['label'].value_counts())
print("\nAWID3, баланс класів:")
print(awid['label'].value_counts())

# Для порівнянності залишаємо у власному наборі тільки Normal і Deauth
# (у AWID3 класу FakeAP немає)
own_bin = own[own['label'].isin(['Normal', 'Deauth'])].copy()

feat_cols = ['total_packets', 'deauth_packets', 'beacon_packets', 'probe_req',
             'probe_resp', 'unique_bssids', 'unique_ssids', 'unique_src',
             'deauth_targets', 'max_bssid_per_ssid', 'deauth_ratio']

X_own, y_own = own_bin[feat_cols], own_bin['label']
X_awid, y_awid = awid[feat_cols], awid['label']


def evaluate(model_name, y_true, y_pred, classes=('Normal', 'Deauth')):
    print(f"\n=== {model_name} ===")
    print(classification_report(y_true, y_pred, zero_division=0))
    cm = confusion_matrix(y_true, y_pred, labels=list(classes))
    print("Матриця помилок:")
    print(cm)
    idx_normal = list(classes).index('Normal')
    actual_normal = cm[idx_normal, :].sum()
    correct_normal = cm[idx_normal, idx_normal]
    fpr = (actual_normal - correct_normal) / actual_normal if actual_normal > 0 else 0
    print(f"[!] FPR (хибні тривоги на Normal): {fpr:.4f} ({fpr*100:.2f}%)")


# ==========================================
# Напрям 1: навчання на власному наборі -> перевірка на AWID3
# ==========================================
print("\n\n--- НАПРЯМ 1: Свій набір -> AWID3 ---")
rf1 = RandomForestClassifier(n_estimators=100, random_state=42)
rf1.fit(X_own, y_own)
pred1 = rf1.predict(X_awid)
evaluate("Random Forest: train=Свій, test=AWID3", y_awid, pred1)

# ==========================================
# Напрям 2: навчання на AWID3 -> перевірка на власному наборі
# ==========================================
print("\n\n--- НАПРЯМ 2: AWID3 -> Свій набір ---")
rf2 = RandomForestClassifier(n_estimators=100, random_state=42)
rf2.fit(X_awid, y_awid)
pred2 = rf2.predict(X_own)
evaluate("Random Forest: train=AWID3, test=Свій", y_own, pred2)
