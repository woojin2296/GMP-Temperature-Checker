#include <iostream>
#include <thread>
#include <future>
#include <chrono>
#include <unistd.h>
#include <wiringPi.h>
#include <wiringPiI2C.h>
#include <ctime>
#include <sqlite3.h>
using namespace std;

// 측정 간격 (초)
#define INTERVAL_SECOND 5

// DHT-22 관련 상수 정의
#define DHT_GPIO_REFRIGERATOR   27      // 냉장용 센서 Data GPIO
#define DHT_GPIO_FREEZER        17      // 냉동용 센서 Data GPIO
#define LH_THRESHOLD            30      // 1-wire 통신 데이터 구분 한계 수치 (마이크로초)

// LCD 관련 상수 정의
#define LCD_ADDR 0x3f                   // 실제 사용하시는 LCD의 I2C 주소로 변경하세요
#define LCD_CHR  1                      // 데이터 모드
#define LCD_CMD  0                      // 명령 모드

#define LCD_BACKLIGHT   0x08            // 백라이트 On
#define ENABLE  0b00000100              // Enable 비트

#define LCD_LINE_1  0x80                // 첫 번째 라인 주소
#define LCD_LINE_2  0xC0                // 두 번째 라인 주소

int lcd_fd; // LCD 파일 디스크립터

struct SensorData {
    int temp;
    int humid;
    bool success;
};

// LCD 함수 선언
void lcd_init(void);
void lcd_byte(int bits, int mode);
void lcd_toggle_enable(int bits);
void lcdLoc(int line); // 커서 위치 설정
void ClrLcd(void); // LCD 화면 지우기
void typeln(const char *s); // 문자열 출력

// DHT-22 함수 선언
SensorData readData(int DHT_GPIO); 

int main() {
    cout << "온습도 센서 데이터 수집 프로그램을 시작합니다." << endl;

    wiringPiSetupGpio();
    piHiPri(99);

    // i2c LCD 초기화
    lcd_fd = wiringPiI2CSetup(LCD_ADDR);
    if (lcd_fd == -1) {
        cout << "i2c LCD 초기화 실패" << endl;
        return 1;
    }
    lcd_init();

    // SQLite 데이터베이스 초기화
    sqlite3 *db;
    int rc = sqlite3_open("ThermoHygrometer.db", &db);
    if(rc){
        cerr << "데이터베이스를 열 수 없습니다: " << sqlite3_errmsg(db) << endl;
        sqlite3_close(db);
        return 1;
    }

    // 테이블 생성
    const char* create_table_sql = 
        "CREATE TABLE IF NOT EXISTS sensor_data ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "timestamp TEXT,"
            "refrigerator_temp REAL,"
            "refrigerator_humid REAL,"
            "freezer_temp REAL,"
            "freezer_humid REAL"
        ");";
    char *errmsg = 0;
    rc = sqlite3_exec(db, create_table_sql, 0, 0, &errmsg);
    if(rc != SQLITE_OK){
        cerr << "테이블 생성 오류: " << errmsg << endl;
        sqlite3_free(errmsg);
        sqlite3_close(db);
        return 1;
    }

    while(true) {
        auto start_time = chrono::steady_clock::now();

        try {
            future<SensorData> refrigeratorFuture = async(launch::async, readData, DHT_GPIO_REFRIGERATOR);
            future<SensorData> freezerFuture = async(launch::async, readData, DHT_GPIO_FREEZER);

            SensorData refrigeratorData = refrigeratorFuture.get();
            SensorData freezerData = freezerFuture.get();

            time_t rawtime;
            time(&rawtime);
            char timestamp[20];
            strftime(timestamp, sizeof(timestamp), "%Y/%m/%d %H:%M:%S", localtime(&rawtime));

            cout << "타임스탬프: " << timestamp << endl;

            if(refrigeratorData.success) {
                cout << "냉장고 센서 - 온도: " << (float)refrigeratorData.temp/10 << "°C, 습도: " << (float)refrigeratorData.humid/10 << "%" << endl;
            } else {
                cout << "냉장고 센서 - 데이터 오류" << endl;
            }

            if(freezerData.success) {
                cout << "냉동고 센서 - 온도: " << (float)freezerData.temp/10 << "°C, 습도: " << (float)freezerData.humid/10 << "%" << endl;
            } else {
                cout << "냉동고 센서 - 데이터 오류" << endl;
            }

            // LCD에 데이터 출력
            ClrLcd();

            char lcd_line1[16];
            char lcd_line2[16];

            if(refrigeratorData.success) {
                snprintf(lcd_line1, sizeof(lcd_line1), "REF %.1fC %.1f%%", refrigeratorData.temp/10.0, refrigeratorData.humid/10.0);
            } else {
                snprintf(lcd_line1, sizeof(lcd_line1), "REF Data Error");
            }

            if(freezerData.success) {
                snprintf(lcd_line2, sizeof(lcd_line2), "FRZ %.1fC %.1f%%", freezerData.temp/10.0, freezerData.humid/10.0);
            } else {
                snprintf(lcd_line2, sizeof(lcd_line2), "FRZ Data Error");
            }

            lcdLoc(LCD_LINE_1);
            typeln(lcd_line1);

            lcdLoc(LCD_LINE_2);
            typeln(lcd_line2);

            // SQLite에 데이터 삽입
            sqlite3_stmt *stmt;
            const char *insert_sql = "INSERT INTO sensor_data (timestamp, refrigerator_temp, refrigerator_humid, freezer_temp, freezer_humid) VALUES (?, ?, ?, ?, ?);";

            rc = sqlite3_prepare_v2(db, insert_sql, -1, &stmt, NULL);
            if(rc != SQLITE_OK){
                cerr << "SQL 문 준비 실패: " << sqlite3_errmsg(db) << endl;
                continue;
            }

            // 바인딩
            rc = sqlite3_bind_text(stmt, 1, timestamp, -1, SQLITE_STATIC);

            if(refrigeratorData.success){
                sqlite3_bind_double(stmt, 2, refrigeratorData.temp/10.0);
                sqlite3_bind_double(stmt, 3, refrigeratorData.humid/10.0);
            } else {
                sqlite3_bind_null(stmt, 2);
                sqlite3_bind_null(stmt, 3);
            }

            if(freezerData.success){
                sqlite3_bind_double(stmt, 4, freezerData.temp/10.0);
                sqlite3_bind_double(stmt, 5, freezerData.humid/10.0);
            } else {
                sqlite3_bind_null(stmt, 4);
                sqlite3_bind_null(stmt, 5);
            }

            rc = sqlite3_step(stmt);
            if(rc != SQLITE_DONE){
                cerr << "데이터 삽입 실패: " << sqlite3_errmsg(db) << endl;
            } else {
                cout << "데이터가 SQLite 데이터베이스에 저장되었습니다." << endl;
            }

            sqlite3_finalize(stmt);

        } catch(const exception& e) {
            cerr << "메인 루프에서 예외 발생: " << e.what() << endl;
            cerr << "다음 루프를 강제 실행합니다" << e.what() << endl;
        }

        // 경과 시간 계산 후 남은 시간 동안 슬립하여 일정한 간격 유지
        auto end_time = chrono::steady_clock::now();
        auto elapsed_ms = chrono::duration_cast<chrono::milliseconds>(end_time - start_time).count();

        if(elapsed_ms < INTERVAL_SECOND * 1000) {
            usleep((INTERVAL_SECOND * 1000 - elapsed_ms) * 1000); // 남은 시간 슬립
        } else {
            // 슬립 필요 없음; 주기보다 오래 걸림
            cout << "주의: 센서 읽기 및 처리 시간이 주기보다 깁니다." << endl;
        }
    }

    // 데이터베이스 연결 종료
    sqlite3_close(db);

    return 0;
}

// DHT-22 데이터 측정 함수
SensorData readData(int DHT_GPIO) {
    SensorData result;
    unsigned char data[5] = {0,0,0,0,0};

    try {
        pinMode(DHT_GPIO, OUTPUT);
        digitalWrite(DHT_GPIO, LOW);
        usleep(18000);
        digitalWrite(DHT_GPIO, HIGH);
        pinMode(DHT_GPIO, INPUT);

        do { delayMicroseconds(1); } while(digitalRead(DHT_GPIO)==HIGH);
        do { delayMicroseconds(1); } while(digitalRead(DHT_GPIO)==LOW);
        do { delayMicroseconds(1); } while(digitalRead(DHT_GPIO)==HIGH);

        for(int d=0; d<5; d++) {
            for(int i=0; i<8; i++) {
                do { delayMicroseconds(1); } while(digitalRead(DHT_GPIO)==LOW);
                int width = 0;
                do {
                    width++;
                    delayMicroseconds(1);
                    if(width>1000) break;
                } while(digitalRead(DHT_GPIO)==HIGH);
                data[d] = data[d] | ((width > LH_THRESHOLD) << (7-i));
            }
        }

        int humid = (data[0]<<8 | data[1]);
        int temp = (data[2]<<8 | data[3]);

        unsigned char chk = 0;
        for(int i=0; i<4; i++){ chk+= data[i]; }
        if(chk==data[4]){
            result.success = true;
            result.temp = temp;
            result.humid = humid;
        } else {
            result.success = false;
            cout << "센서 " << DHT_GPIO << " 읽기 중 오류 발생: 체크섬 불일치" << endl;
        }

    } catch(const exception& e) {
        result.success = false;
        cout << "센서 " << DHT_GPIO << " 읽기 중 오류 발생: " << e.what() << endl;
    }

    return result;
}

// LCD 초기화 함수
void lcd_init() {
    // LCD 초기화 시퀀스
    lcd_byte(0x33, LCD_CMD); // 초기화
    lcd_byte(0x32, LCD_CMD); // 초기화
    lcd_byte(0x06, LCD_CMD); // 커서 이동 방향
    lcd_byte(0x0C, LCD_CMD); // 디스플레이 On, 커서 Off, 블링크 Off
    lcd_byte(0x28, LCD_CMD); // 데이터 길이, 라인 수, 폰트 크기
    lcd_byte(0x01, LCD_CMD); // 디스플레이 지우기
    usleep(500);
}

// LCD에 데이터 또는 명령어 전송
void lcd_byte(int bits, int mode) {
    int bits_high;
    int bits_low;
    // 상위 4비트와 하위 4비트를 분리하여 전송
    bits_high = mode | (bits & 0xF0) | LCD_BACKLIGHT ;
    bits_low = mode | ((bits<<4) & 0xF0) | LCD_BACKLIGHT ;
    // 상위 비트 전송
    wiringPiI2CWrite(lcd_fd, bits_high);
    lcd_toggle_enable(bits_high);
    // 하위 비트 전송
    wiringPiI2CWrite(lcd_fd, bits_low);
    lcd_toggle_enable(bits_low);
}

// Enable 신호 토글
void lcd_toggle_enable(int bits) {
    usleep(500);
    wiringPiI2CWrite(lcd_fd, (bits | ENABLE));
    usleep(500);
    wiringPiI2CWrite(lcd_fd, (bits & ~ENABLE));
    usleep(500);
}

// 특정 위치로 커서 이동
void lcdLoc(int line) {
    lcd_byte(line, LCD_CMD);
}

// LCD 화면 지우기
void ClrLcd(void) {
    lcd_byte(0x01, LCD_CMD);
    lcd_byte(0x02, LCD_CMD);
}

// 문자열 출력
void typeln(const char *s) {
    while(*s) lcd_byte(*(s++), LCD_CHR);
}