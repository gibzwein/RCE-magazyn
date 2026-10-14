# RCE Magazyn

Sterownik magazynu energii dla ESP32. Program pobiera ceny RCE z API PSE, wyznacza godzinowy plan ładowania i rozładowania magazynu oraz wyświetla bieżącą rekomendację w monitorze portu szeregowego.

## Funkcje

- Pobiera ceny RCE na dziś i — od godziny 13:00 — na jutro.
- Uśrednia ceny 15-minutowe do wartości godzinowych, uwzględniając doby 23-, 24- i 25-godzinne.
- Wyznacza plan pracy magazynu algorytmem programowania dynamicznego.
- Udostępnia konfigurację przez portal Wi-Fi Manager.
- Zapisuje parametry w pamięci ESP32.
- Ponawia nieudane pobrania cen.

## Wymagania

- Płytka ESP32; kod rozpoznaje też wariant ESP32-C3 i dobiera pin przycisku BOOT.
- Arduino IDE lub inne środowisko obsługujące Arduino Core dla ESP32.
- Biblioteki z Menedżera bibliotek:
  - WiFiManager (tzapu), około 2.0.17
  - ArduinoJson, wersja 7.x

Pozostałe użyte biblioteki (`WiFi`, `WiFiClientSecure`, `HTTPClient`, `Preferences`, `time`) są częścią środowiska ESP32.

## Wgrywanie i pierwsze uruchomienie

1. Otwórz `rce_magazyn.ino` w Arduino IDE.
2. Wybierz właściwą płytkę ESP32 i port szeregowy.
3. Skompiluj i wgraj program.
4. Połącz się z siecią Wi-Fi skonfigurowaną wcześniej w urządzeniu.
5. Otwórz monitor portu szeregowego z prędkością **115200 bit/s**.

Przy pierwszym uruchomieniu lub gdy nie ma zapisanych danych Wi-Fi urządzenie uruchomi portal konfiguracyjny o nazwie `RCE-Magazyn`. Połącz się z tą siecią i otwórz adres `192.168.4.1`, aby skonfigurować Wi-Fi oraz parametry algorytmu.

Portal można otworzyć także przyciskiem **BOOT**: przytrzymaj go przez około 1,5–2 sekundy po uruchomieniu urządzenia lub podczas jego pracy. Nie przytrzymuj BOOT w trakcie resetowania — może to wprowadzić ESP32 w tryb programowania.

## Parametry algorytmu

Wartości początkowe:

| Parametr | Wartość początkowa | Znaczenie |
| --- | ---: | --- |
| Sprawność obiegu | 88% | Udział energii odzyskiwanej podczas rozładowania |
| Próg opłacalności | 100 zł/MWh | Minimalny zysk wymagany dla godziny rozładowania |
| Pojemność | 4 h | Liczba godzin pracy z pełną mocą w jednym kierunku |
| Limit ładowania | 8 h/dobę | Maksymalna liczba godzin ładowania w ciągu doby |

Każda zaplanowana godzina oznacza ładowanie pełną mocą, rozładowanie pełną mocą albo bezczynność. Plan rozpoczyna dobę z pustym magazynem, a energia pozostawiona na koniec doby nie jest wyceniana.

## Dane i bezpieczeństwo

Ceny są pobierane z publicznego API PSE: `https://api.raporty.pse.pl/api/rce-pln`. Dane Wi-Fi są wprowadzane lokalnie przez portal Wi-Fi Manager i przechowywane przez ESP32 w pamięci urządzenia — nie są wpisane do pliku źródłowego.

Nie dodawaj haseł ani innych danych dostępowych do repozytorium. Pliki takie jak `secrets.h`, `secrets.local.h` i `credentials.h` są ignorowane przez Git.

## Uwaga

Program wyświetla rekomendacje w monitorze szeregowym. Sam kod nie steruje bezpośrednio falownikiem ani sprzętem magazynu energii.
