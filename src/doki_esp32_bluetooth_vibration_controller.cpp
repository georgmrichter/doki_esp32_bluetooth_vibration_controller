#include <cstdarg>
#include <queue>

#include <SPI.h>
#include <Arduino.h>
#include <Wire.h>
#include "BluetoothSerial.h"
#include "Adafruit_DRV2605.h"
#include "XT_DAC_Audio.h"
#include "driver/dac.h"

#define DAC_PIN 25
#define DAC_TIMER 0
#define BUFFER_SIZE 100
#define CHUNK_QUEUE_TARGET_SIZE 5

#define REG_AC_COUPLE DRV2605_REG_CONTROL1
#define REG_N_PWM_ANALOG DRV2605_REG_CONTROL3

#define BIT_AC_COUPLE 0x20
#define BIT_N_PWM_ANALOG 0xA3

enum PacketType {
	Test = 1,
	Status = 2,
	VibrationEffect = 3,
	VibrationPattern = 4,
	VibrationSound = 5,
	VibrationSoundRequest = 6,
	VibrationRealtime = 7
};

BluetoothSerial SerialBT;
Adafruit_DRV2605 drv;
XT_DAC_Audio_Class dacAudio(DAC_PIN, DAC_TIMER);

void serialPrintlnf(int length, const char *format, ...);

class WavChunk{
public:
	int32_t soundId;
	int32_t chunkId;
	char* wavData;
	XT_Wav_Class* wav;

	WavChunk(int32_t soundId, int32_t chunkId, char* data, int32_t dataLength){
		this->soundId = soundId;
		this->chunkId = chunkId;
		this->wavData = new char[dataLength];
		memcpy(this->wavData, data, dataLength);
		this->wav = new XT_Wav_Class((unsigned char*)this->wavData);
		this->wav->Volume = 1;
	}

	~WavChunk(){
		delete[] this->wavData;
		delete wav;
	}
};

std::queue<WavChunk> chunkQueue;
bool audioMode = false;
hw_timer_t* chunkBufferTimer = nullptr;
hw_timer_t* audioPlaybackTimer = nullptr;
bool exitAudiomodeFlag = false;
uint8_t regACCouple = 0;
uint8_t regPWMAnalog = 0;

bool patternMode = false;
char* currentPattern = nullptr;
int patternIt = 1;
int patternLength = 0;
bool playNextPatternFlag = false;

bool realTimeMode = false;

enum PatternStatus {
	Play = 1,
	Stop = 2,
	Resume = 3
};

void setup();
void loop();
void readPacketFromBT();
void handlePacket(int32_t packetType, char *data);
void handleTestPacket(char *data);
void handleVibrationPacket(char* data);
void handleVibrationPatternPacket(char* data);
void replacePattern(char* data);
void deletePattern();
void handleVibrationSound(char* data);
void handleVibrationSoundRequest(char* data);
void handleRealtime(char* data);
void playVibrationEffect(uint8_t effectId);
void playVibrationEffects(int32_t count, char* values);
void IRAM_ATTR onChunkBufferTimer();
void IRAM_ATTR onAudioPlaybackTimer();
void requestNewAudioChunk();
char getPatternSplitTime(char* data);

void setup()
{
	Serial.begin(115200);
	SerialBT.begin("ESP32BluetoothTest");

	drv.begin();
	drv.selectLibrary(1);
	// I2C trigger by sending 'go' command 
  	// default, internal trigger when sending GO command
  	drv.setMode(DRV2605_MODE_INTTRIG);
	drv.useERM();
	regPWMAnalog = drv.readRegister8(REG_N_PWM_ANALOG);
	regACCouple = drv.readRegister8(REG_AC_COUPLE);

	dacAudio.DacVolume = 1;
	dac_output_enable(DAC_CHANNEL_1);
	float voltage = 3.3f;
	float desiredVoltage = 1.8f;
	float factor = desiredVoltage / voltage;
	uint8_t dacValue = factor * 255 - 1;
	dac_output_voltage(DAC_CHANNEL_1, dacValue);

	chunkBufferTimer = timerBegin(1, 80, true);
	timerAttachInterrupt(chunkBufferTimer, &onChunkBufferTimer, true);
	int timerIntervalMs = 200;
	timerAlarmWrite(chunkBufferTimer, timerIntervalMs * 1000, true);

	audioPlaybackTimer = timerBegin(2, 80, true);
	timerAttachInterrupt(audioPlaybackTimer, &onAudioPlaybackTimer, true);
	timerIntervalMs = 10;
	timerAlarmWrite(audioPlaybackTimer, timerIntervalMs * 1000, true);

	Serial.println();
	Serial.println("Setup complete.");
}

void loop()
{
	// Exiting audio mode in an interrupt causes a crash.
	// This is probably because the chunk queue is cleared,
	// which may contain large amounts of data.
	// Because of that (or something else) this takes too long
	// to be executed on an interrupt.
	// So instead this flag is used to perform the operation
	// on the next main loop.
	if(exitAudiomodeFlag){
		int32_t request = 2;
		handleVibrationSoundRequest((char*)&request);
		exitAudiomodeFlag = false;
	}
	if(playNextPatternFlag){
		if(patternIt < patternLength) {
			drv.setRealtimeValue(currentPattern[patternIt]);
			patternIt += 2;
			playNextPatternFlag = false;
		}
	}
	if (SerialBT.available())
	{
		readPacketFromBT();
	}
	dacAudio.FillBuffer();
	yield();
	//delay(25);
}

void readPacketFromBT(){
	int intSize = sizeof(int32_t);
	if(SerialBT.available() < 2 * intSize) return;
	int32_t packetType = 0;
	int32_t dataLength = 0;
	SerialBT.readBytes((char*)&packetType, intSize);
	SerialBT.readBytes((char*)&dataLength, intSize);
	if(dataLength == 0 || packetType == 0) return;
	char* data = new char[dataLength];
	serialPrintlnf(50, "Expecting %d Bytes", dataLength);
	int read = SerialBT.readBytes(data, dataLength);

	serialPrintlnf(50, "Bytes recieved: %d", read);
	handlePacket(packetType, data);
	delete[] data;
}

void serialPrintlnf(int length, const char *format, ...)
{
	va_list args;
	va_start(args, format);

	char *buffer = new char[length];
	vsprintf(buffer, format, args);
	Serial.println(buffer);

	delete[] buffer;
	va_end(args);
}

void handlePacket(int32_t packetType, char *data)
{
	switch (packetType)
	{
	case PacketType::Test:
		handleTestPacket(data);
		break;
	case PacketType::VibrationEffect:
		handleVibrationPacket(data);
		break;
	case PacketType::VibrationPattern:
		handleVibrationPatternPacket(data);
		break;
	case PacketType::VibrationSound:
		handleVibrationSound(data);
		break;
	case PacketType::VibrationSoundRequest:
		handleVibrationSoundRequest(data);
		break;
	case PacketType::VibrationRealtime:
		handleRealtime(data);
		break;
	}
}

void handleRealtime(char* data){
	int16_t* rawValue = (int16_t*)data;
	uint8_t value = *rawValue;
	serialPrintlnf(50, "realtime value: %d", value);
	if(!realTimeMode) {
		drv.setMode(DRV2605_MODE_REALTIME);
		realTimeMode = true;
	}
	drv.setRealtimeValue(value);
}

void handleTestPacket(char *data)
{
	int32_t *length = (int32_t *)data;
	serialPrintlnf(50, "length: %d", *length);
	data += sizeof(int32_t);
	Serial.println(data);
}

void handleVibrationPacket(char* data){
	int32_t* length = (int32_t*)data;
	data += sizeof(int32_t);
	realTimeMode = false;
	drv.setMode(DRV2605_MODE_INTTRIG);
	playVibrationEffects(*length, data);
}

void handleVibrationPatternPacket(char* data){
	int32_t* status = (int32_t*)data;
	data += sizeof(int32_t);
	switch(*status){
		case PatternStatus::Play:
		{
			patternMode = true;
			replacePattern(data);
			int timerIntervalMs = getPatternSplitTime(data);
			timerAlarmWrite(audioPlaybackTimer, timerIntervalMs * 1000, true);
			drv.setMode(DRV2605_MODE_REALTIME);
			if(audioMode){
				int32_t request = 2;
				handleVibrationSoundRequest((char*)&request);
			}
			timerAlarmEnable(audioPlaybackTimer);
			break;
		}
		case PatternStatus::Stop:
		{
			timerAlarmDisable(audioPlaybackTimer);
			break;
		}
		case PatternStatus::Resume:
		{
			if(currentPattern == nullptr) break;
			patternMode = true;
			drv.setMode(DRV2605_MODE_REALTIME);
			timerAlarmEnable(audioPlaybackTimer);
			break;
		}
	}
}

char getPatternSplitTime(char* data){
	// skip length
	data += sizeof(int32_t);
	//skip first value
	data += sizeof(char);
	return *data;
}

void deletePattern(){
	if(currentPattern != nullptr){
		delete[] currentPattern;
		patternIt = 1;
		patternLength = 0;
	}
}

void replacePattern(char* data){
	deletePattern();
	int32_t* length = (int32_t*)data;
	data += sizeof(int32_t);
	patternLength = *length;
	currentPattern = new char[patternLength];
	memcpy(currentPattern, data, patternLength);
}

void handleVibrationSound(char* data){
	int32_t* soundFile = (int32_t*)data;
	data += sizeof(int32_t);
	int32_t* chunkId = (int32_t*)data;
	data += sizeof(int32_t);
	int32_t* dataLength = (int32_t*)data;
	data += sizeof(int32_t);
	serialPrintlnf(100, "Vibration sound %d %d %d", *soundFile, *chunkId, *dataLength);

	WavChunk* chunk = new WavChunk(*soundFile, *chunkId, data,* dataLength);
	chunkQueue.push(*chunk);

	if(realTimeMode){
		realTimeMode = false;
	}
	if(patternMode){
		deletePattern();
		patternMode = false;
	}
	if(!audioMode) {
		int timerIntervalMs = 10;
		timerAlarmWrite(audioPlaybackTimer, timerIntervalMs * 1000, true);
		drv.setMode(DRV2605_MODE_AUDIOVIBE);
		//drv.writeRegister8(REG_AC_COUPLE, regACCouple | (1UL << BIT_AC_COUPLE));
		//drv.writeRegister8(REG_N_PWM_ANALOG, regACCouple | (1UL << BIT_N_PWM_ANALOG));
		drv.writeRegister8(REG_AC_COUPLE, BIT_AC_COUPLE);
		drv.writeRegister8(REG_N_PWM_ANALOG, BIT_N_PWM_ANALOG);
		dacAudio.FillBuffer();
		dacAudio.Play(chunkQueue.front().wav);
		audioMode = true;
		timerAlarmEnable(chunkBufferTimer);
		timerAlarmEnable(audioPlaybackTimer);
	}
}

void handleVibrationSoundRequest(char* data){
	int32_t* requestType = (int32_t*)data;
	if(*requestType == 2){
		timerAlarmDisable(chunkBufferTimer);
		timerAlarmDisable(audioPlaybackTimer);
		dacAudio.StopAllSounds();
		drv.setMode(DRV2605_MODE_INTTRIG);
		drv.writeRegister8(REG_AC_COUPLE, regACCouple);
		drv.writeRegister8(REG_N_PWM_ANALOG, regPWMAnalog);
		audioMode = false;
		chunkQueue = std::queue<WavChunk>(); // clear the queue
	}
}

void playVibrationEffects(int32_t count, char* values){
	if(count > 8) return;
	drv.stop();
	for(int i = 0; i < count; i++){
		drv.setWaveform(i, values[i]);
		serialPrintlnf(25, "effect %d", values[i]);
	}
	if(count < 8){
	for(int i = count; i < 8; i++){
		drv.setWaveform(i, 0);
	}
	}
	drv.go();
}

void playVibrationEffect(uint8_t effectId){
	drv.stop();
	drv.setWaveform(0, effectId);
	drv.setWaveform(1, 0);
	drv.go();
}

int it1 = 0;
int it2 = 0;

void IRAM_ATTR onChunkBufferTimer(){
	if(!audioMode) return;
	serialPrintlnf(50, "onChunkBufferTimer %d", it1++);
	int diff = CHUNK_QUEUE_TARGET_SIZE - chunkQueue.size();
	serialPrintlnf(50, "chunk diff: %d", diff);
	for (int i = 0; i < diff; i++)
	{
		requestNewAudioChunk();
	}
}

void IRAM_ATTR onAudioPlaybackTimer(){
	if(audioMode) {
		if(chunkQueue.empty()) return;
		//if(dacAudio.FirstPlayListItem == nullptr){
		if(!chunkQueue.front().wav->Playing){
			Serial.println("Playing new chunk");
			chunkQueue.pop();
			if(chunkQueue.empty()){
				exitAudiomodeFlag = true;
				Serial.println("Chunk queue is empty. Exiting audio vibration mode.");
				return;
			}
			dacAudio.Play(chunkQueue.front().wav);
		}
	}
	if(patternMode) {
		playNextPatternFlag = true;
	}
}

void requestNewAudioChunk(){
	int32_t packet[3] = {
		PacketType::VibrationSoundRequest,
		(int32_t)sizeof(int32_t),
		1
	};
	SerialBT.write((uint8_t*)packet, sizeof(packet));
}