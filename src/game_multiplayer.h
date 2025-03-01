#ifndef EP_GAME_MULTIPLAYER_H
#define EP_GAME_MULTIPLAYER_H

#include <string>
#include "game_system.h"
#include <lcf/rpg/eventpage.h>

namespace Game_Multiplayer {
	void Connect(int map_id);
	void Quit();
	void Update();
	void MainPlayerMoved(int dir);
	void MainPlayerChangedMoveSpeed(int spd);
	void MainPlayerChangedSpriteGraphic(std::string name, int index);
	void SePlaySync(const lcf::rpg::Sound& sound);
	void WeatherEffectSync(int type, int sthrength);
	void VariableSync(int32_t id, int32_t val);
	void SwitchSync(int32_t id, int32_t val);
	void AnimTypeSync(lcf::rpg::EventPage::AnimType animtype);
	void AnimFrameSync(uint16_t frame);
	void FacingSync(uint16_t facing);
	void SetTypingStatus(uint16_t status);
	void FlashSync(int r, int g, int b, int p, int t);

	void FlashAll(int r, int g, int b, int p, int t);
	void TintAll();
}

#endif
