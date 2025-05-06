
#include <csignal>
#include <atomic>
#include <iostream>
#include <map>
#include <cstdint>
#include <string>
#include <cstring>
#include <mosquitto.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <ctime>

using json = nlohmann::json;

// Константы времени:
constexpr int MQTT_LOOP_DELAY = 100;    // Таймаут ожидания сетевой активности для mosquitto_loop

// Добавляем отдельный флаг для graceful shutdown
std::atomic<bool> shouldExit(false);    // Для штатного завершения
std::atomic<bool> shouldRestart(false); // Для перезапуска

// Получение переменных окружения со значениями по умолчанию
std::string getEnvVar(const char* name, const char* defaultValue) {
    const char* value = std::getenv(name);
    return value ? value : defaultValue;
}
//-------------------------------------------------------------------------------------------------

// Код вспомогательных классов помещается в один файл исключительно в целях и рамках тестового задания

///@brief ООП-обертка над Mosquitto
class MosquittoClient {
public:
    using MessageCallback   = std::function<void(const std::string& topic, const json& message)>;
    using LogCallback       = std::function<void(int level, const std::string& message)>;

    MosquittoClient(const std::string& clientId)
        : mosq_(nullptr), userMessageCallback(nullptr), userLogCallback(nullptr), clientId_(clientId)
    {
        mosquitto_lib_init();
        mosq_ = mosquitto_new(clientId.c_str(), true, this);
        if (!mosq_) {
            throw std::runtime_error("Failed to create mosquitto instance");
        }

        mosquitto_connect_callback_set   (mosq_, &MosquittoClient::onConnect);
        mosquitto_disconnect_callback_set(mosq_, &MosquittoClient::onDisconnect);
        mosquitto_message_callback_set   (mosq_, &MosquittoClient::onMessage);
        mosquitto_log_callback_set       (mosq_, &MosquittoClient::onLog);

        // Автоматическое переподключение с экспоненциальной задержкой
        mosquitto_reconnect_delay_set(mosq_, 1, 30, true);
    }

    ~MosquittoClient() {
        if (mosq_) {
            mosquitto_disconnect(mosq_);
            mosquitto_destroy(mosq_);
        }
        mosquitto_lib_cleanup();
    }

    bool connect() {
        // Получение учетных данных из переменных окружения
        std::string mqttHost     = getEnvVar("MQTT_HOST", "localhost");
        int         mqttPort     = std::stoi(getEnvVar("MQTT_PORT", "1883"));
        std::string mqttUsername = getEnvVar("MQTT_USERNAME", "");
        std::string mqttPassword = getEnvVar("MQTT_PASSWORD", "");

        if ( isConnected_ ) return true;
        
        std::cout << "Connecting to MQTT broker at " << mqttHost << ":" << mqttPort << std::endl;
        
        // Установка учетных данных
        if (!mqttUsername.empty() && !mqttPassword.empty()) {
            mosquitto_username_pw_set(mosq_, mqttUsername.c_str(), mqttPassword.c_str());
        }
        
        // Подключение к брокеру
        int rc = mosquitto_connect(mosq_, mqttHost.c_str(), mqttPort, 60);
        if (rc != MOSQ_ERR_SUCCESS) {
            std::cerr << "Unable to connect to MQTT broker: " << mosquitto_strerror(rc) << std::endl;
            return false;
        }
        
        return true;
    }

    bool restart() {
        // 1. Освобождаем текущие ресурсы
        if (mosq_) {
            mosquitto_disconnect(mosq_);
            mosquitto_destroy(mosq_);
            mosq_   = nullptr;
        }
        mosquitto_lib_cleanup();

        // 2. Инициализируем библиотеку заново
        mosquitto_lib_init();

        // 3. Создаём новый экземпляр клиента
        mosq_ = mosquitto_new(clientId_.c_str(), true, this);
        if (!mosq_) {
            std::cerr << "[MQTT] Failed to create mosquitto instance during restart" << std::endl;
            return false;
        }

        // 4. Устанавливаем callback-и
        mosquitto_connect_callback_set   (mosq_, &MosquittoClient::onConnect);
        mosquitto_disconnect_callback_set(mosq_, &MosquittoClient::onDisconnect);
        mosquitto_message_callback_set   (mosq_, &MosquittoClient::onMessage);
        mosquitto_log_callback_set       (mosq_, &MosquittoClient::onLog);

        // 5. Настраиваем переподключение
        mosquitto_reconnect_delay_set(mosq_, 1, 30, true);

        // 6. Пытаемся подключиться
        if ( !connect() ) {
            std::cerr << "[MQTT] Connect failed during restart..." << std::endl;
            return false;
        }

        std::cout << "[MQTT] Restart successful" << std::endl;
        return true;
    }

    void loop(int timeout_ms = 100) {
        int rc = mosquitto_loop(mosq_, timeout_ms, 1);
        if (rc != MOSQ_ERR_SUCCESS) {
            std::cerr << "Mosquitto loop error: " << mosquitto_strerror(rc) << std::endl;
        }
    }

    bool subscribe(const std::string& topic, int qos = 0) {
        int rc = mosquitto_subscribe(mosq_, nullptr, topic.c_str(), qos);
        if (rc != MOSQ_ERR_SUCCESS) {
            std::cerr << "Mosquitto subscribe error: " << mosquitto_strerror(rc) << std::endl;
            return false;
        }
        std::cout << "Successfully subscribed to " << topic << std::endl;
        return true;
    }

    bool publish(const std::string& topic, const json& message, int qos = 1, bool retain = false) {
        auto str = message.dump();
    
        constexpr int MAX_PUBLISH_ATTEMPTS   = 3;
        constexpr int PUBLISH_RETRY_DELAY_MS = 100;
    
        for (int attempt = 1; attempt <= MAX_PUBLISH_ATTEMPTS; ++attempt) {
            // По умолчанию, QoS=1 - для гарантированной доставки
            int rc = mosquitto_publish(mosq_, nullptr, topic.c_str(),
                                        static_cast<int>(str.size()), str.c_str(), qos, retain);
            if (rc == MOSQ_ERR_SUCCESS) {
                std::cout << "Successfully published MQTT message" << std::endl;
                return true;
            }
    
            std::cerr << "[MQTT] Publish attempt " << attempt << " failed: "
                        << mosquitto_strerror(rc) << std::endl;
    
            if (attempt < MAX_PUBLISH_ATTEMPTS) {
                // Вместо простого sleep вызываем loop, чтобы libmosquitto могла обработать события
                mosquitto_loop(mosq_, 10, 1);
                std::this_thread::sleep_for(std::chrono::milliseconds(PUBLISH_RETRY_DELAY_MS));
            }
        }
    
        std::cerr << "[MQTT] Failed to publish message after "
                    << MAX_PUBLISH_ATTEMPTS << " attempts." << std::endl;
        return false;
    }

    void setMessageCallback(MessageCallback cb) {
        userMessageCallback = std::move(cb);
    }

    void setLogCallback(LogCallback cb) {
        userLogCallback = std::move(cb);
    }

    bool isConnected() { return isConnected_; }

private:
    struct mosquitto*   mosq_;
    MessageCallback     userMessageCallback;
    LogCallback         userLogCallback;
    std::string         clientId_;
    bool                isConnected_    = false;

    static void onConnect(struct mosquitto* mosq_, void* obj, int rc) {
        auto self = static_cast<MosquittoClient*>(obj);
        if (rc == MOSQ_ERR_SUCCESS) {
            self->isConnected_    = true;
            std::cout << "[MQTT] Connected successfully" << std::endl;
        } else {
            std::cerr << "[MQTT] Connect failed: " << mosquitto_strerror(rc) << std::endl;
        }
    }

    static void onDisconnect(struct mosquitto* mosq_, void* obj, int rc) {
        auto self = static_cast<MosquittoClient*>(obj);
        self->isConnected_    = false;
        std::cout << "[MQTT] Disconnected with code " << rc << std::endl;
    }

    static void onMessage(struct mosquitto* mosq_, void* obj, const struct mosquitto_message* msg) {
        auto self = static_cast<MosquittoClient*>(obj);
        if (!self->userMessageCallback) return;

        if (!msg->payload) {
            std::cout << "Received empty message" << std::endl;
            return;
        }

        try {
            std::string payload_str(static_cast<char*>(msg->payload), msg->payloadlen);
            json j = json::parse(payload_str);
            self->userMessageCallback(msg->topic, j);
        } catch (const json::parse_error& e) {
            std::cerr << "[MQTT] JSON parse error: " << e.what() << " at byte " << e.byte << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[MQTT] JSON error: " << e.what() << std::endl;
        }
    }

    static void onLog(struct mosquitto* mosq_, void* obj, int level, const char* str) {
        auto self = static_cast<MosquittoClient*>(obj);
        if (self->userLogCallback) {
            self->userLogCallback(level, str);
        } else {
            std::cerr << "[MQTT LOG] " << str << std::endl;
        }
    }
};
//-------------------------------------------------------------------------------------------------    

// Определение класса работы с портами ввода-вывода:

// Человеко-читаемые названия пинов (для приложения)
enum class Gpio : uint8_t {
    Button      = 2,
    Led         = 13,
    Red         = 3,
    Green       = 5,
    Blue        = 6,
    TempSensor  = 36,                   // A0

    _COUNT      = 40                    // Ограничение - max кол-во пинов ESP32
};

// Режимы работы пина
enum class PinMode : uint8_t {
    INPUT,
    OUTPUT,
    ANALOG_INPUT,
    ANALOG_OUTPUT,
    PWM
};
    
///@brief Адам для всех пинов
class GpioPinBase {
public:
    static constexpr bool isValid(Gpio pin) {
        return static_cast<uint8_t>(pin) < static_cast<uint8_t>(Gpio::_COUNT);
    }

protected:
    Gpio    pin_;
    PinMode mode_;

    constexpr GpioPinBase(Gpio pin, PinMode mode) : pin_(pin), mode_(mode) {}
};
    
///@brief Пользовательский класс для статического создания пинов (динамическое - чуть менее эффективно)
// Используем protected наследование - базовый класс скрыт от внешнего кода
template <Gpio Pin, PinMode M>
class GpioPin : protected GpioPinBase {
    static_assert(GpioPinBase::isValid(Pin), "Invalid GPIO pin");

public:
    GpioPin() : GpioPinBase(Pin, M) {
        std::cout << "Pin " << (int)pin_ << " set to " << 
        (mode_==PinMode::INPUT          ? "INPUT": 
         mode_==PinMode::OUTPUT         ? "OUTPUT": 
         mode_==PinMode::ANALOG_INPUT   ? "ANALOG_INPUT": 
         mode_==PinMode::ANALOG_OUTPUT  ? "ANALOG_OUTPUT": 
         "PWM") << std::endl;
    }

    // Цифровая запись - только для OUTPUT
    void writeDigital(bool value) {
        static_assert(M == PinMode::OUTPUT, "write(bool) allowed only in OUTPUT mode");
        std::cout << "Digital write to pin " << static_cast<int>(Pin) << ": " << (value ? "HIGH" : "LOW") << "\n";
        state_.digitalValue = value;
    }
    
    // Цифровая запись-переключение - только для OUTPUT
    void toggle() {
        static_assert(M == PinMode::OUTPUT, "write(bool) allowed only in OUTPUT mode");
        state_.digitalValue = !state_.digitalValue;
        std::cout << "Digital toggle pin " << static_cast<int>(Pin) << ": " << (state_.digitalValue ? "HIGH" : "LOW") << "\n";
    }

    // Аналоговая запись - для ANALOG_OUTPUT и PWM
    void writeAnalog(uint8_t value) {
        static_assert(M == PinMode::ANALOG_OUTPUT || M == PinMode::PWM,
                        "write(uint8_t) allowed only in ANALOG_OUTPUT or PWM mode");
        std::cout << "Analog/PWM write to pin " << static_cast<int>(Pin) << ": " << static_cast<int>(value) << "\n";
        state_.analogValue = value;
    }

    // Цифровое чтение - только для INPUT
    bool readDigital() const {
        static_assert(M == PinMode::INPUT, "read() allowed only in INPUT mode");
        std::cout << "Digital read from pin " << static_cast<int>(Pin) << ": " << (state_.digitalValue ? "HIGH" : "LOW") << "\n";
        return state_.digitalValue;
    }

    // Аналоговое чтение - только для ANALOG_INPUT
    uint8_t readAnalog() const {
        static_assert(M == PinMode::ANALOG_INPUT, "readAnalog() allowed only in ANALOG_INPUT mode");
        std::cout << "Analog read from pin " << static_cast<int>(Pin) << ": " << static_cast<int>(state_.analogValue) << "\n";
        return state_.analogValue;
    }

    // Для эмуляции: возможность напрямую изменить дискретное значение
    void setDigitalValue(bool value) {
        state_.digitalValue = value;
    }
    bool getDigitalValue() {
        return state_.digitalValue;
    }

    // Для эмуляции: возможность напрямую изменить аналоговое значение
    void setAnalogValue(uint8_t value) {
        state_.analogValue = value;
    }
    uint8_t getAnalogValue() {
        return state_.analogValue;
    }
    
    // Техническая возможность именять направление пина динамически
    // но в данной реализации класса, после смены направления не будет контроля 
    // над операций чтения/записи и направленности пина (т.к. оно проверяется в статике)
    void setMode(PinMode newMode) {
        std::cout << "Pin " << (int)pin_ << " set to " << 
        (mode_==PinMode::INPUT          ? "INPUT": 
         mode_==PinMode::OUTPUT         ? "OUTPUT": 
         mode_==PinMode::ANALOG_INPUT   ? "ANALOG_INPUT": 
         mode_==PinMode::ANALOG_OUTPUT  ? "ANALOG_OUTPUT": 
         "PWM") << std::endl;
         
         mode_ = newMode;
        // Здесь можно добавить логику инициализации под новый режим
    }

private:
    // Состояние пина (выделено в отдельную структуру с оглядкой на исходную реализацию - pinStates)
    struct PinState {
        bool    digitalValue    = false;
        uint8_t analogValue     = 0;
    } state_;
};
//-------------------------------------------------------------------------------------------------

///@brief Класс для кнопки (цифровой вход)
class Button {
public:
    Button() : pin_() {}

    bool isPressed() const {
        return pin_.readDigital();      // Активный Hi
    }

    void update() {
        pin_.setDigitalValue( (std::rand() % 2) == 1 );
        // Можно добавить обработку дребезга, таймеры и т.п.
    }

    void reset () {
        std::cout << "Reseting Button" << std::endl;
        // Можно добавить аппаратную настройку
    }

private:
    GpioPin<Gpio::Button, PinMode::INPUT> pin_;
};

///@brief Класс для светодиода (цифровой выход)
class Led {
public:
    Led() : pin_() {}

    void on() {
        pin_.writeDigital(true);
    }

    void off() {
        pin_.writeDigital(false);
    }

    void toggle() {
        pin_.toggle();
    }

    void update() {
        // Можно добавить анимации, мигание и т.п.
    }

    void publishState(MosquittoClient& mqtt) {
        std::string emb_pin_stat    = "embedded/pins/state";
        json payload = { {"pin", Gpio::Led}, {"value", pin_.getDigitalValue()} };

        std::cout << "Publishing MQTT message to topic " << emb_pin_stat << ": " << payload << std::endl;

        mqtt.publish(emb_pin_stat, payload);    // отправка с повторными попытками при неудаче
    }

    void reset () {
        std::cout << "Reseting Led" << std::endl;
        off();
        // Можно добавить аппаратную настройку
    }

private:
    GpioPin<Gpio::Led, PinMode::OUTPUT> pin_;
};

///@brief Класс для датчика температуры (аналоговый вход)
class TemperatureSensor {
public:
    TemperatureSensor() : pin_() {}

    uint8_t readRaw() const {
        return pin_.readAnalog();
    }

    float readCelsius() const {
        // Пример преобразования, зависит от датчика
        return (readRaw() / 255.0f) * 100.0f;
    }

    void update() {
        pin_.setAnalogValue( 20 + (std::rand() % 11) );
        // Можно добавить фильтрацию, усреднение и т.п.
    }

    void publishState(MosquittoClient& mqtt) {
        std::string emb_sens_temp    = "embedded/sensors/temperature";

        auto now     = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastSendTime_);

        if (elapsed.count() >= 5) {  
            // В задании не указан формат для отправки температуры, поэтому 
            // использую формат состояния пина (только аналогового)          
            json payload = { {"pin", Gpio::TempSensor}, {"value", readRaw()} };
            
            mqtt.publish(emb_sens_temp, payload);

            std::cout << "Publishing MQTT message to topic " << emb_sens_temp << ": " << payload << std::endl;

            lastSendTime_ = now;        // Обновляем время последней отправки
        }
    }

    void reset () {
        std::cout << "Reseting Temperature Sensor" << std::endl;
        pin_.setAnalogValue(0);
        // Можно добавить аппаратную настройку
    }

private:
    GpioPin<Gpio::TempSensor, PinMode::ANALOG_INPUT> pin_;
    std::chrono::steady_clock::time_point lastSendTime_;
};

///@brief Класс для PWM-устройства (например, RGB-светодиод один канал)
class PwmDevice {
public:
    explicit PwmDevice() : pin_() {}

    void write(uint8_t value) {
        pin_.writeAnalog(value);
    }

    void update() {
        // Можно добавить плавное изменение яркости и т.п.
    }

    void reset () {
        std::cout << "Reseting PwmDevice" << std::endl;
        write(0);
    }

private:
    // Для универсальности можно сделать шаблон с параметром Gpio
    // Здесь для примера фиксируем Gpio::Green
    GpioPin<Gpio::Green, PinMode::PWM> pin_;
};

///@brief Класс для RGB-светодиода (составной)
class RgbLed {
public:
    RgbLed()
        : redPin_(), greenPin_(), bluePin_()
    {}

    void setColor(uint8_t r, uint8_t g, uint8_t b) {
        redPin_.writeAnalog(r);
        greenPin_.writeAnalog(g);
        bluePin_.writeAnalog(b);
    }

    void update() {
        // Можно добавить эффекты, плавные переходы и т.п.
    }

    void reset() {
        std::cout << "Reseting RgbLed pins" << std::endl;
        setColor(0, 0, 0);
        // Можно сделать еще что-то...
    }

    void publishState(MosquittoClient& mqtt) {
        std::string emb_pin_stat    = "embedded/pins/state";
        json payload = { 
            {"red",    redPin_  .getAnalogValue()},
            {"green",  greenPin_.getAnalogValue()},
            {"blue",   bluePin_ .getAnalogValue()}
        };

        std::cout << "Publishing MQTT message to topic " << emb_pin_stat << ": " << payload << std::endl;

        mqtt.publish(emb_pin_stat, payload);
    }

private:
    GpioPin<Gpio::Red,   PinMode::PWM> redPin_;
    GpioPin<Gpio::Green, PinMode::PWM> greenPin_;
    GpioPin<Gpio::Blue,  PinMode::PWM> bluePin_;
};
//-------------------------------------------------------------------------------------------------

///@brief Класс приложения

class Application {
public:
    Application() : mqttClient("embedded-controller"),
                    button(),
                    led(),
                    rgbLed(),
                    tempSensor(),
                    pwmDevice()
    {}

    void setup() {
        // Для корректной работы rand
        std::srand(static_cast<unsigned int>(std::time(nullptr)));
        // Настройка MQTT, подписки, инициализация устройств:
        // Настройка пинов
        button.reset();
        led.reset();
        rgbLed.reset();
        tempSensor.reset();
        pwmDevice.reset();

        // Настройка MQTT
        mqttClient.setMessageCallback([this](const std::string& topic, const nlohmann::json& msg) {
            this->mqttMessageHandler(topic, msg);
        });

        mqttClient.setLogCallback([this](int level, const std::string& msg) {
            this->mqttLogHandler(level, msg);
        });

        if (mqttClient.connect()) {
            mqttClient.subscribe("embedded/control");            
        } else {
            // Обработка ошибки, повторная попытка подключения и/или выход
            return;
        }

        std::cout << "Setup completed" << std::endl;
    }

    void restart() {
        mqttClient.restart();
        setup();
    }
    
    // Главный цикл программы
    void loop() {
        // Проверок на подключение не делается, т.к. переподключение происходит автоматически по 
        // установленным в конструкторе параметрам (1 - 30сек, экспоненциально).
    
        // Задаем таймаут ожидания сетевой активности и не делаем дополнительной задержки в loop
        // т.к. mosquitto_loop() использует системный вызов select() для ожидания сетевой активности 
        // с заданным таймаутом. Это значит, что поток засыпает на время ожидания.
        mqttClient.loop(MQTT_LOOP_DELAY);

        // Если получена команда перезапуска
        if (shouldRestart) {
            return;
        }

        // Логика обновления/вычисления/циклических действий:
        button.update();
        if ( button.isPressed() ) 
            led.toggle();

        rgbLed.update();

        tempSensor.update();
        tempSensor.publishState(mqttClient);

        pwmDevice.update();             // Этот просто за компанию

    }

    void cleanup() {
        // Очистка ресурсов, если нужно
        std::cout << "Cleaning up resources..." << std::endl;
    }

private:
    MosquittoClient     mqttClient;
    Button              button;
    Led                 led;
    RgbLed              rgbLed;
    TemperatureSensor   tempSensor;
    PwmDevice           pwmDevice;

    std::string         emb_ctrl    = "embedded/control";
    std::string         emb_err     = "embedded/errors";
    std::string         cmd_fld     = "command";
    std::string         cmd_rst     = "restart";
    std::string         cmd_set_rgb = "set_rgb";

    void mqttMessageHandler(const std::string& topic, const nlohmann::json& message) {
        // Обработка сообщений MQTT
        std::cout << "[MQTT] Message received on topic: " << topic << ", payload: " << std::endl;
        std::cout << message.dump(2) << std::endl;

        // Логика обработки
        if (topic == emb_ctrl) {
            if (message.contains(cmd_fld)) {
                std::string command = message[cmd_fld];
                // Команда Сброса
                if (command == cmd_rst) {
                    std::cout << "Received command: " << cmd_rst << std::endl;
                    // Изменяем состояние пина 2 (Led) перед перезапуском
                    led.toggle();               // Инвертируем текущее состояние
                    led.publishState(mqttClient);
                    shouldRestart = true;
                    return;
                    // В исходном коде было так:
                    // Изменяем состояние пина 2 перед перезапуском
                    // bool currentState = digitalRead(2);
                    // digitalWrite(2, !currentState); // Инвертируем текущее состояние
                    // Зачем и как записывать в Кнопку новое состояние (или тут какой-то хитрый замысел)?
                }
                // Команда Установки цветов RGB
                if (command == cmd_set_rgb) {
                    std::cout << "Received command: " << cmd_set_rgb << std::endl;
                    
                    // Изменяем состояние RGB Led
                    if (!message.contains("red") || !message.contains("green") || !message.contains("blue")) {
                        json err = {{"error", "Missing RGB fields"}};
                        mqttClient.publish(emb_err, err);
                        return;
                    }
                    int r = message["red"];
                    int g = message["green"];
                    int b = message["blue"];

                    if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) {
                        json err = {{"error", "Invalid RGB values"}};
                        mqttClient.publish(emb_err, err);
                        return;
                    }

                    std::cout << "Set RGB to (" << r << ", " << g << ", " << b << ")\n";

                    rgbLed.setColor(r, g, b);
                    rgbLed.publishState(mqttClient);
                    return;
                }
            } else {
                std::cout << "Missing or invalid 'command' field" << std::endl;
                mqttClient.publish(emb_err, "Missing or invalid 'command' field");
            }
        }
    }

    /// @brief Простой логер взаимодействия с MQTT-брокером
    /// @param level   уровень логирования
    /// @param message сообщение (log)
    void mqttLogHandler(int level, const std::string& message) {
        switch(level) {
            case MOSQ_LOG_INFO:
                std::cout << "[MQTT INFO] " << message << std::endl;
                break;
            case MOSQ_LOG_NOTICE:
                std::cout << "[MQTT NOTICE] " << message << std::endl;
                break;
            case MOSQ_LOG_WARNING:
                std::cerr << "[MQTT WARNING] " << message << std::endl;
                break;
            case MOSQ_LOG_ERR:
                std::cerr << "[MQTT ERROR] " << message << std::endl;
                break;
            default:
                std::cout << "[MQTT LOG] " << message << std::endl;
                break;
        }
    }
    
};
//-------------------------------------------------------------------------------------------------

///@brief Обработчики сигналов
/// Используются для корректного завершения программы и для решения вопроса утечки ресурсов
/// т.к. код после цикла while был недостижим.
void handleShutdown(int sig) {
    std::cout << "\nReceived shutdown signal: " << sig << std::endl;
    shouldExit = true;
}

void handleRestart(int sig) {
    std::cout << "\nReceived restart signal: " << sig << std::endl;
    shouldRestart = true;
}
//-------------------------------------------------------------------------------------------------

// 
int main() {
    // Настройка обработчиков сигналов
    signal(SIGINT, handleShutdown);   // Ctrl+C -> shutdown
    signal(SIGTERM, handleShutdown);  // systemctl stop -> shutdown
    signal(SIGHUP, handleRestart);    // Перезагрузка через systemctl reload
    
    Application app;

    app.setup();

    while (!shouldExit) {
        app.loop();

        if (shouldRestart) {
            shouldRestart = false;
            std::cout << "Restarting application..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(3));
            app.restart();
        }
    }

    app.cleanup();

    std::cout << "Application exited gracefully." << std::endl;
    return 0;
}
//-------------------------------------------------------------------------------------------------
