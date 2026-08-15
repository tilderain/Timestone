#include "eNotifManager.h"
#include <iostream>
#include <Windows.h>
#include "../imgui/imgui.h"
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

eNotificationManager* Notifications = new eNotificationManager();

void eNotificationManager::Init()
{
	m_bIsNotificationActive = false;
	m_tTimer = GetTickCount() & 0x7FFFFFFF;
}

void eNotificationManager::Update()
{
	if (GetTickCount64() - m_tTimer <= m_nNotificationTime) return;
	m_tTimer = GetTickCount() & 0x7FFFFFFF;

	m_bIsNotificationActive = false;
}

void eNotificationManager::Draw()
{
	if (m_bIsNotificationActive)
	{
		ImDrawList* drawList = ImGui::GetForegroundDrawList();
		const ImGuiStyle& style = ImGui::GetStyle();
		const ImVec2 position(5.0f, 10.0f);
		const ImVec2 textSize = ImGui::CalcTextSize(szMessageBuffer);
		const ImVec2 size(ImGui::GetIO().DisplaySize.x - 15.0f,
			textSize.y + style.WindowPadding.y * 2.0f);
		const ImVec2 end(position.x + size.x, position.y + size.y);

		drawList->AddRectFilled(position, end,
			ImGui::GetColorU32(ImGuiCol_WindowBg), style.WindowRounding);
		if (style.WindowBorderSize > 0.0f)
			drawList->AddRect(position, end,
				ImGui::GetColorU32(ImGuiCol_Border), style.WindowRounding,
				0, style.WindowBorderSize);
		drawList->AddText(
			ImVec2(position.x + style.WindowPadding.x,
			       position.y + style.WindowPadding.y),
			ImGui::GetColorU32(ImGuiCol_Text), szMessageBuffer);
	}
}

void eNotificationManager::SetNotificationTime(int time)
{
	m_nNotificationTime = time;
}

void eNotificationManager::PushNotification(const char * format, ...)
{
	va_list args;
	va_start(args, format);
	vsprintf(szMessageBuffer, format, args);
	va_end(args);
	m_bIsNotificationActive = true;
}
