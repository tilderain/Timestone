//TODO: split this into multiple files and put the methods in a cpp file
#pragma once
#include "../MemoryMgr.h"
#include "../utils/Trampoline.h"
#include <vector>
using namespace Memory::VP;

#define longlong  long long
#define ulonglong  unsigned long long
#define undefined8  long long*
#define undefined7  long long
#define undefined2  int

typedef int (*code)(longlong* param_1);
typedef int (*codenoarg)();
typedef void (*method)(void);
#define undefined int
#define undefined4 int
#define CONCAT71(a,b) (a<<32 | b)
#define backupsize 10000//100000
#define prebackup 0 //100000

constexpr int backupSize = 1000;
constexpr int teamBackupSize = 0x960;

int runaheadcount = 0;
bool pAddrSet = false;

bool saveMemoryInited;
int currentSaveLoadOffset;
std::vector<uint8_t> SaveMemory;


struct MarvelVector {
	float x;
	float y;
	float xVelocity;
};
enum EGroundedState :uint8_t {
	NoGround = 0,
	Grounded = 0xF4,
	InAir = 0xF2,
};

enum ETagState :uint8_t {
	NoTag = 0,
	Active = 0x0D,
	Inactive = 0x09,
	Active2 = 0x68,
	TagginOut = 0xB4,
	TagginIn = 0x6c
};

struct Fighter { // TODO map this out
	char offset[0x14];
	ETagState tagState;
	char offset2[0x3B];
	MarvelVector vector;
	char offset3[0x10];
	EGroundedState  groundedState;
	char offset4[0xc7];
	uint16_t specialState;//specialstate?
	char offset5[0x528];
	uint32_t anim; // anim??
};

struct ScriptableFighter {
	Fighter* fighter = 0;
	intptr_t fighterController = 0; // TODO find better name for this
	char* name = nullptr;
	uintptr_t tickPtr = 0x14004bd30;
	void Tick() {
		((void(__fastcall*)(longlong*))_addr(tickPtr))((longlong*)fighter);
	}
	const char* GetGroundedState() {
		switch (fighter->groundedState)
		{
		case Grounded:
			return "Grounded";
		case InAir:
			return "InAir";
		default:
			return "Unkown";
			break;
		}
	}
	const char* GetTagState() {
		switch (fighter->tagState)
		{
		case Active:
		case Active2:
			return "Active";
		case Inactive:
			return "Inactive";
		case TagginOut:
			return "TagginOut";
		case TagginIn:
			return "TagginIn";
		default:
			return "Unkown";
			break;
		}
	}
};

struct physics_t
{
	bool inited = false;
	longlong param_1;
};
physics_t physics;


ScriptableFighter scriptableFighters[6] = { 0 };


bool paused = false;;

void hook(long long addr) {
	((void(__fastcall*)())_addr(addr))();
}


template<typename A>
void hook(long long addr, A a) {
	((void(__fastcall*)(A))_addr(addr))(a);
}

template<typename A, typename B>
void hook(long long addr, A a, B b) {
	((void(__fastcall*)(A, B))_addr(addr))(a, b);
}
template<typename A, typename B, typename C>
void hook(long long addr, A a, B b, C c) {
	((void(__fastcall*)(A, B, C))_addr(addr))(a, b, c);
}

template<typename A, typename B, typename C, typename D>
void hook(long long addr, A a, B b, C c, D d) {
	((void(__fastcall*)(A, B, C, D))_addr(addr))(a, b, c, d);
}


template<typename X>
X hookret(long long addr) {
	return ((X(__fastcall*)())_addr(addr))();
}

template<typename X, typename A>
X hookret(long long addr, A a) {
	return ((X(__fastcall*)(A))_addr(addr))(a);
}

template<typename X, typename A, typename B>
X hookret(long long addr, A a, B b) {
	return ((X(__fastcall*)(A, B))_addr(addr))(a, b);
}
template<typename X, typename A, typename B, typename C>
X hookret(long long addr, A a, B b, C c) {
	return ((X(__fastcall*)(A, B, C))_addr(addr))(a, b, c);
}
template<typename X, typename A, typename B, typename C, typename D>
X hookret(long long addr, A a, B b, C c, D d) {
	return ((X(__fastcall*)(A, B, C, D))_addr(addr))(a, b, c, d);
}


uintptr_t getPointerOffset(uintptr_t ptr, std::vector<unsigned int> offsets) {
	uintptr_t retval = ptr;
	for (unsigned int i = 0; i < offsets.size(); ++i)
	{
		auto val = (uintptr_t*)(ptr + offsets[i]);
		retval = *val;
	}
	return retval;
}

uintptr_t FindDMAAddy(uintptr_t ptr, std::vector<unsigned int> offsets)
{
	auto hProc = GetModuleHandle(nullptr);
	uintptr_t addr = ptr;
	for (unsigned int i = 0; i < offsets.size(); ++i)
	{
		ReadProcessMemory(hProc, (BYTE*)addr, &addr, sizeof(addr), 0);
		addr += offsets[i];
	}
	return addr;
}


void SaveLoadPtr(intptr_t ptr, bool save, int size) {
	if (!saveMemoryInited) {
		SaveMemory.resize(SaveMemory.size() + size);
	}
	if (save)
		memcpy(&SaveMemory[currentSaveLoadOffset], (uint8_t*)ptr, size);
	else
		memcpy((uint8_t*)ptr, &SaveMemory[currentSaveLoadOffset], size);
	currentSaveLoadOffset += size;
}

void SaveLoadState(bool save) {

	currentSaveLoadOffset = 0;
	auto mysterytable = *(uintptr_t*)_addr(0x140d533e0);
	auto block2 = *(uintptr_t*)_addr(0x140d47e68);
	if (!pAddrSet) {
		for (size_t i = 0; i < 6; i++)
		{
			scriptableFighters[i].fighter = *(Fighter**)(mysterytable + 0xaa0 + (0x438 * i));
			pAddrSet = true;
		}
	}
	for (size_t i = 0; i < 6; i++)
	{
		SaveLoadPtr((intptr_t)scriptableFighters[i].fighter, save, backupSize);
		SaveLoadPtr((intptr_t)scriptableFighters[i].fighter + 0x538, save, backupSize);
		SaveLoadPtr((intptr_t)scriptableFighters[i].fighter + 0x18, save, backupSize);
		SaveLoadPtr((intptr_t)scriptableFighters[i].fighter + 0x6d8, save, backupSize);
		SaveLoadPtr((intptr_t)scriptableFighters[i].fighter + 0x858, save, backupSize);
		SaveLoadPtr((intptr_t)scriptableFighters[i].fighter + 0x9d8, save, backupSize);
		SaveLoadPtr((intptr_t)scriptableFighters[i].fighter + 0xb58, save, backupSize);


		

	}
	SaveLoadPtr((intptr_t)(mysterytable),save,9200);
	auto team1 = block2 + 0x350;
	auto team2 = block2 + 0x610;
	SaveLoadPtr(block2, save, 2272);
	//don't think these 2 are needed anymore
	SaveLoadPtr(team1, save, teamBackupSize);
	SaveLoadPtr(team2, save, teamBackupSize);
	if(physics.inited)
		SaveLoadPtr((intptr_t)physics.param_1, save, 11424);
	saveMemoryInited = true;
}


void LoadState() {
	SaveLoadState(false);
}




void SaveState() {

	SaveLoadState(true);
}


// Fetch physics parameters and just hook forward
struct physicsY_t
{
	bool inited = false;
	longlong param_1; longlong* param_2; longlong param_3; longlong* param_4; longlong param_5;
};
physicsY_t physicsY;
struct physicsX_t
{
	bool inited = false;
	longlong* param_1; undefined2* param_2; longlong* param_3; longlong* param_4;
	longlong param_5; undefined* param_6; undefined* param_7; undefined* param_8;
	longlong* param_9;
};
physicsX_t physicsX;
//physicsY
longlong* FUN_1400199c0(longlong param_1, longlong* param_2, longlong param_3, longlong* param_4, longlong param_5) {
	if (!physicsY.inited)
		printf("physY %x %x %x %x %x\n", param_1, param_2, param_3, param_4, param_5);
	physicsY.inited = true;
	physicsY.param_1 = param_1;
	physicsY.param_2 = param_2;
	physicsY.param_3 = param_3;
	physicsY.param_4 = param_4;
	physicsY.param_5 = param_5;

	return ((longlong * (*)(longlong, longlong*, longlong, longlong*, longlong))_addr(0x1400199c0))(param_1, param_2, param_3, param_4, param_5);
}


//AnmEditorSprite
undefined8* FUN_14045d860(undefined8* param_1, char* param_2, longlong param_3, ulonglong param_4, int param_5,int param_6, uint8_t param_7) {
	printf("%s %llx %llx\n", param_2,param_1,param_3);
	return ((undefined8 * (*)(undefined8 * , char* , longlong , ulonglong , int , int , uint8_t))_addr(0x14045d860))(param_1, param_2, param_3, param_4, param_5,param_6,param_7);
}


undefined8* FUN_14001c210(undefined8* param_1, char* param_2, char* param_3, undefined8 param_4,
	uint16_t param_5) {
	//printf("%s %llx\n", param_3,param_4);
	return ((undefined8 * (*)(undefined8 *, char*, char*, undefined8,uint16_t))_addr(0x14001c210))(param_1, param_2, param_3, param_4, param_5);
}

uintptr_t inputRef;
bool inputRefSet = false;

bool recording = false;
bool replaying = false;
bool replayAvailable = false;
int recordReplayIndex = 0;
int recordedLength = 0;
#define ReplayLength (60*120)
#define ReplayBufferSize 550
uint8_t replayBuffer[ReplayLength][ReplayBufferSize];
uint8_t replayBuffer2[ReplayLength][ReplayBufferSize];

void FUN_1402b41b0(longlong param_1)
{
	printf("replay %x\n", param_1);
	if (!inputRefSet) {
		inputRefSet = true;
		inputRef = (uintptr_t)param_1;
		printf("replayu %x\n", param_1);
	}

	((void (*)(longlong))_addr(0x1402b41b0))(param_1);
	if (inputRefSet) {
		if (recording) {
			printf("recording %d\n", recordReplayIndex);
			memcpy(&replayBuffer[recordReplayIndex], (uint8_t*)inputRef, ReplayBufferSize);
			memcpy(&replayBuffer2[recordReplayIndex], (uint8_t*)inputRef+ 0x2c0, ReplayBufferSize);
			recordReplayIndex++;
			replayAvailable = true;
			if (recordReplayIndex >= ReplayLength -1) {
				recording = false;
				recordedLength = recordReplayIndex - 1;
			}
		}
		if (replaying) {
			printf("replaying %d\n", recordReplayIndex);
			memcpy((uint8_t*)inputRef, &replayBuffer[recordReplayIndex], ReplayBufferSize);
			memcpy((uint8_t*)inputRef+ 0x2c0, &replayBuffer2[recordReplayIndex], ReplayBufferSize);
			recordReplayIndex++;
			if (recordReplayIndex >= recordedLength) {
				replaying = false;
			}
		}
	}
}

void  FUN_140521df0(longlong param_1)
{
	if (!physics.inited)
		printf("phys:%x",param_1);
	physics.inited = true;
	physics.param_1 = param_1;
	((void (*)(longlong))_addr(0x140521df0))(param_1);
}

undefined2* FUN_1405ab9a0(longlong* param_1, undefined2* param_2, longlong* param_3, longlong* param_4,
	longlong param_5, undefined* param_6, undefined* param_7, undefined* param_8,
	longlong* param_9) {
	if(!physicsX.inited)
		printf("physX %x %x %x %x %x\n", param_1, param_2, param_3, param_4, param_5, param_6, param_7, param_8,
		param_9);
	physicsX.inited = true;
	physicsX.param_1 = param_1;
	physicsX.param_2 = param_2;
	physicsX.param_3 = param_3;
	physicsX.param_4 = param_4;
	physicsX.param_5 = param_5;
	physicsX.param_6 = param_6;
	physicsX.param_7 = param_7;
	physicsX.param_8 = param_8;
	physicsX.param_9 = param_9;
	return ((undefined2 * (*)(longlong*, undefined2*, longlong*, longlong*,
		longlong, undefined*, undefined*, undefined*,
		longlong*))_addr(0x11405ab9a0))(param_1, param_2, param_3, param_4,
			param_5, param_6, param_7, param_8,
			param_9);
}





void playercode(longlong* param_1) {
	//if (!pAddrSet) {
	//	pAddrSet = true;
	//	auto mysterytable = *(uintptr_t*)_addr(0x140d533e0);
	//	auto block2 = *(uintptr_t*)_addr(0x140d47e68);
	//	for (size_t i = 0; i < 6; i++)
	//	{
	//		scriptableFighters[i].fighter = *(uintptr_t*)(mysterytable + 0xaa0 + (0x438 * i));
	//		printf("p%d:%x\n", i, scriptableFighters[i]);
	//	}
	//	SaveState();
	//}

	//if ((intptr_t)param_1 == scriptableFighters[0].fighter) {
	//	for (size_t x = 0; x < 6; x++)
	//	{
	//		if (x == 0)LoadState();
	//		for (size_t i = 0; i < runaheadcount; i++)
	//		{

	//			scriptableFighters[i].Tick();
	//			/*auto posY = (float*)pAddresses[i].ptr + 0x54;
	//			auto groundedEnum = (uint8_t*)pAddresses[i].ptr + 0x6c;
	//			if (*posY > 0) {
	//				*posY -= 10.0f;
	//				*groundedEnum = 0xF2;

	//			}
	//			if (*posY < 0) {
	//				*posY = 0.0f;
	//			}
	//			if (*posY == 0) {
	//				*groundedEnum = 0xF4;
	//			}*/
	//		}
	//		if (physics.inited) {
	//			((void (*)(longlong, longlong, undefined8, longlong))_addr(0x1402594a0))(physics.param_1,5,0,5);
	//		}
	//		if (physicsY.inited) {
	//			((longlong * (__fastcall*)(longlong, longlong*, longlong, longlong*, longlong))_addr(0x1400199c0))(physicsY.param_1, physicsY.param_2, physicsY.param_3, physicsY.param_4, physicsY.param_5);
	//		}
	//		/*		if (physicsX.inited) {
	//			((undefined2 * (*)(longlong*, undefined2*, longlong*, longlong*,
	//				longlong, undefined*, undefined*, undefined*,
	//				longlong*))_addr(0x11405ab9a0))(physicsX.param_1, physicsX.param_2, physicsX.param_3, physicsX.param_4,
	//					physicsX.param_5, physicsX.param_6, physicsX.param_7, physicsX.param_8,
	//					physicsX.param_9);
	//		}*/
	//		if (x == 0)SaveState();
	//	}
	//	//		auto t4 = getPointerOffset((uintptr_t)baseaddr(0xD76108), { 0 ,0x98,0x28,0x30,0xc0,0 });
	//			//((void(*)(longlong , undefined8 , undefined8 , undefined8))_addr(0x1402594a0))(t4, (undefined8)0, (undefined8)0, (undefined8)0);

	//	printf("=====\n");
	//}
}



void PREPHYSICS(longlong param_1, undefined8 param_2, undefined8 param_3, undefined8 param_4) { //for auto-gen code
	printf("prephys: %x %x %x %x \n", param_1, param_2, param_3, param_4);
}

void POSTPHYSICS() {} //for auto-gen code




bool simulation = false;

void chartick(longlong* param1, intptr_t methaddr, const char* name) {
	{ // save data for current fighter. TODO once this code goes trough all fighters, stop executing
		auto mysterytable = *(uintptr_t*)_addr(0x140d533e0);
		auto block2 = *(uintptr_t*)_addr(0x140d47e68);
		bool hasbeenset = false;
		for (size_t i = 0; i < 6; i++)
		{
			scriptableFighters[i].fighter = *(Fighter**)(mysterytable + 0xaa0 + (0x438 * i));
			if (scriptableFighters[i].fighter == (Fighter*)param1) {
				scriptableFighters[i].tickPtr = methaddr;
				hasbeenset = true;
				if (scriptableFighters[i].name == nullptr) {
					scriptableFighters[i].name = new char[strlen(name) + 1] {0};
					memcpy(scriptableFighters[i].name, name, strlen(name));
				}
			}
		}
		if (!pAddrSet) {
			SaveState();
			pAddrSet = true;
		}
		if (!hasbeenset) {
			printf("unknownaddr:%x - %s\n", (uintptr_t)param1, name);
		}
	}
	if (replaying) {
		memcpy((uint8_t*)inputRef + 0x2c0, &replayBuffer2[recordReplayIndex], ReplayBufferSize);
	}

	if (paused) return; // Returning here will should (in theory) pause the game 

	if (runaheadcount == 0) { // if not rollbacking just execute the method and stop the rest.
		((void(__fastcall*)(longlong*))_addr(0x14004bd30))((longlong*)param1);
		return;
	}

	if (!simulation) { // if not in simulation mode & this is the handle for the first fighter run enable runahead and run update method for each fighter again in a runahead amount of for loop. If simulation enabled run the base update method;
		if ((Fighter*)param1 == scriptableFighters[0].fighter) {
			simulation = true;
			for (size_t x = 0; x < runaheadcount; x++)
			{
				if (x == 0)LoadState();
				for (size_t i = 0; i < 6; i++)
				{
					scriptableFighters[i].Tick();
					/*if (physics.inited) {
						((void (__fastcall*)(longlong))_addr(0x140521df0))(physics.param_1);
					}*/
					//Physics doesn't work during runahead; This should in theory fix it. But doesn't
				/*	if (physicsY.inited) {
						((longlong * (__fastcall*)(longlong, longlong*, longlong, longlong*, longlong))_addr(0x1400199c0))(physicsY.param_1, physicsY.param_2, physicsY.param_3, physicsY.param_4, physicsY.param_5);*/
						//}
						/*
						if (scriptableFighters[i].fighter->vector.y > 0) {
							scriptableFighters[i].fighter->vector.y -= 10.0f;
							scriptableFighters[i].fighter->groundedState = EGroundedState::Grounded;
						}
						if (scriptableFighters[i].fighter->vector.y < 0) {
							scriptableFighters[i].fighter->vector.y = 0;
						}
						if (scriptableFighters[i].fighter->vector.y == 0) {
							scriptableFighters[i].fighter->groundedState = EGroundedState::Grounded;
						}

								if (physicsX.inited) {
									((undefined2 * (*)(longlong*, undefined2*, longlong*, longlong*,
										longlong, undefined*, undefined*, undefined*,
										longlong*))_addr(0x11405ab9a0))(physicsX.param_1, physicsX.param_2, physicsX.param_3, physicsX.param_4,
											physicsX.param_5, physicsX.param_6, physicsX.param_7, physicsX.param_8,
											physicsX.param_9);
								}*/
					if (x == 0)SaveState();
				}
			}



			simulation = false;
		}
	}
	else {
		((void(__fastcall*)(longlong*))_addr(0x14004bd30))((longlong*)param1);
	}
}