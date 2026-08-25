#pragma once
#include <cstdint>

class eNotificationManager {
public:
	char szMessageBuffer[2048] = {};
	bool m_bIsNotificationActive;
	uint64_t m_nNotificationTime;
	uint64_t m_tTimer;


	void Init();

	void Update();
	void Draw();
	void SetNotificationTime(int time);
	void PushNotification(const char* format, ...);
};

extern eNotificationManager* Notifications;
