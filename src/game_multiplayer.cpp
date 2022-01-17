#include <map>
#include <memory>
#include <queue>
#include <set>
#include <emscripten/emscripten.h>
#include <emscripten/websocket.h>

#include "game_multiplayer.h"
#include "chat_multiplayer.h"
#include "output.h"
#include "game_player.h"
#include "sprite_character.h"
#include "window_base.h"
#include "drawable_mgr.h"
#include "scene.h"
#include "bitmap.h"
#include "font.h"
#include "input.h"
#include "nxjson.h"
#include "cache.h"
#include "game_screen.h"
#include "game_variables.h"
#include "game_switches.h"
#include "game_map.h"
#include "player.h"

/*
	================
	NAMETAG RENDERER
	================
*/

// one Drawable is responsible for drawing all name tags,
// so that it can control how to render when multiple nametags intersect.
class DrawableNameTags : public Drawable {
	struct Tag {
		BitmapRef renderGraphic;
		Game_Character* anchor;
		int x = 0;
		int y = 0;
	};
	struct TagStack {
		std::vector<Tag*> stack;
		float swayAnim = 0;
	};
	// Store nametags indexable by UID.
	// Also store list of nametags per tile, so they're drawn stacked on each other by iterating through occupied tiles.
	std::map<std::string, std::unique_ptr<Tag>> nameTags; // tags indexed by UID.
	std::unordered_map<unsigned long, TagStack> nameStacks; // list of tags per tile.
																	 // key is a hash based on tile coordinates, and value is a list of nametags on that tile.

	unsigned long coordHash(int x, int y) { // perfect hash for coordinate pairs
		// uniquely map integers to whole numbers
		unsigned long a = 2*x;
		if(x < 0) { a = -2*x-1; }
		unsigned long b = 2*y;
		if(y < 0) { b = -2*y-1; }
		// Cantor pairing function
		return ((a+b)*(a+b+1))/2+b;
	}

	void buildTagGraphic(Tag* tag, std::string name) {
		Rect rect = Font::Tiny()->GetSize(name);
		tag->renderGraphic = Bitmap::Create(rect.width+1, rect.height+1);
		Color shadowColor = Color(0, 0, 0, 255); // shadow color
		Text::Draw(*tag->renderGraphic, 1, 1, *Font::Tiny(), shadowColor, name); // draw black fallback shadow
		Text::Draw(*tag->renderGraphic, 0, 0, *Font::Tiny(), *Cache::SystemOrBlack(), 0, name);
	}
public:
	DrawableNameTags() : Drawable(Priority_Window, Drawable::Flags::Global) {
		DrawableMgr::Register(this);
	}

	void Draw(Bitmap& dst) {
		const unsigned int stackDelta = 8;
		for(auto& it : nameStacks) {
			unsigned int nTags = it.second.stack.size();
			if(nTags >= 10) {
				// swaying
				it.second.swayAnim += 0.01;
			} else {
				it.second.swayAnim = 0;
			}
			for(int i = 0; i < nTags; i++) {
				Tag* tag = it.second.stack[i];
				auto rect = tag->renderGraphic->GetRect();
				if(it.second.swayAnim == 0) {
					// steady stack
					dst.Blit(tag->anchor->GetScreenX()-rect.width/2, tag->anchor->GetScreenY()-rect.height-TILE_SIZE*1.75-stackDelta*i, *tag->renderGraphic, rect, Opacity::Opaque());
				} else {
					// jenga tower be like
					float sway = sin(it.second.swayAnim);
					float swayRadius = 192/sway;
					float angleStep = stackDelta/swayRadius;
					int rx = tag->anchor->GetScreenX();
					int ry = tag->anchor->GetScreenY()-TILE_SIZE*1.75;
					rx = rx+(cos(i*angleStep)-1)*swayRadius;
					ry = ry-sin(i*angleStep)*swayRadius;
					float angle = -angleStep*i;
					dst.RotateZoomOpacityBlit(rx, ry, rect.width/2, rect.height, *tag->renderGraphic, rect, angle, 1, 1, Opacity::Opaque());
				}
			}
		}
	}

	void createNameTag(std::string uid, Game_Character* anchor) {
		assert(!nameTags.count(uid)); // prevent creating nametag for uid that already exists

		std::unique_ptr<Tag> tag = std::make_unique<Tag>();
		tag->anchor = anchor;
		buildTagGraphic(tag.get(), "");

		nameStacks[coordHash(tag->x, tag->y)].stack.push_back(tag.get());
		nameTags[uid] = std::move(tag);
	}

	void deleteNameTag(std::string uid) {
		assert(nameTags.count(uid)); // prevent deleting non existent nametag

		Tag* tag = nameTags[uid].get();
		std::vector<Tag*>& stack = nameStacks[coordHash(tag->x, tag->y)].stack;
		stack.erase(std::remove(stack.begin(), stack.end(), tag), stack.end());
		nameTags.erase(uid);
	}

	void clearNameTags() {
		nameTags.clear();
		nameStacks.clear();
	}

	void moveNameTag(std::string uid, int x, int y) {
		assert(nameTags.count(uid)); // prevent moving non existent nametag

		Tag* tag = nameTags[uid].get();
		std::vector<Tag*>& stack = nameStacks[coordHash(tag->x, tag->y)].stack;
		stack.erase(std::remove(stack.begin(), stack.end(), tag), stack.end());
		tag->x = x;
		tag->y = y;
		nameStacks[coordHash(tag->x, tag->y)].stack.push_back(tag);
	}

	void setTagName(std::string uid, std::string name) {
		assert(nameTags.count(uid)); // prevent using non existent nametag
		buildTagGraphic(nameTags[uid].get(), name);
	}
};

std::unique_ptr<DrawableNameTags> nameTagRenderer; //global nametag renderer

/*
	================
	================
	================
*/

void ConnectToGame();

struct MPPlayer {
	std::queue<std::pair<int,int>> mvq; //queue of move commands
	std::shared_ptr<Game_PlayerOther> ch; //character
	uint16_t typingstatus;
	//this one is used to save player speed before setting it to max speed when move queue is too long
	int moveSpeed;
	std::unique_ptr<Sprite_Character> sprite;
};

std::string multiplayer__my_name = "";

namespace {

	namespace PacketTypes {
		const uint16_t movement = 1;
		const uint16_t sprite = 2;
		const uint16_t sound = 3;
		const uint16_t weather = 4;
		const uint16_t name = 5;
		const uint16_t movementAnimationSpeed = 6;
		const uint16_t variable = 7;
		const uint16_t switchsync = 8;
		const uint16_t animtype = 9;
		const uint16_t animframe = 10;
		const uint16_t facing = 11;
		const uint16_t typingstatus = 12;
		const uint16_t syncme = 13;
	};

	namespace MultiplayerSettings {
		uint8_t playersVolume = 50;
		//adding delay before setting weather since i dont fucking understand when it sets weather on entering another room
		uint8_t weatherSetDelay = 25;
		uint8_t weatherT = 0;
		int nextWeatherType = -1;
		int nextWeatherStrength = -1;
	
		int switchsync = 0;
		std::set<int> syncedswitches = std::set<int>();
		std::set<int> switchlogblacklist = std::set<int>(); //set of switch ids that souldn't be logged

		std::string spritesheet = "";
		int spriteid = 0;

		bool shouldsync = false;

		time_t lastConnect = 0;
		time_t reconnectInterval = 5; //5 seconds

	}
	bool firstRoomUpdate = true;

	std::unique_ptr<Window_Base> conn_status_window;
	//const std::string server_url = "wss://dry-lowlands-62918.herokuapp.com/";
	std::string server_url = "";
	EMSCRIPTEN_WEBSOCKET_T socket;
	bool connected = false;
	std::string myuuid = "";
	int room_id = -1;
	std::map<std::string,MPPlayer> players;
	const std::string delimchar = "\uffff";

	#define SEND_BUFFER_SIZE 2048
	#define RECEIVE_BUFFER_SIZE 8192
	char sendBuffer[SEND_BUFFER_SIZE];
	char receiveBuffer[RECEIVE_BUFFER_SIZE];

	void TrySend(std::string msg) {
		if (!connected) return;
		unsigned short ready;
		emscripten_websocket_get_ready_state(socket, &ready);
		if (ready == 1) { //1 means OPEN
			emscripten_websocket_send_binary(socket, (void*)msg.c_str(), msg.length());
		}

		MultiplayerSettings::shouldsync = true;
	}

	void TrySend(const void* buffer, size_t size) {
		if (!connected) return;
		unsigned short ready;
		emscripten_websocket_get_ready_state(socket, &ready);
		if (ready == 1) { //1 means OPEN
			emscripten_websocket_send_binary(socket, (void*)buffer, size);
		}

		MultiplayerSettings::shouldsync = true;	
	}

	void SetConnStatusWindowText(std::string s) {
		conn_status_window->GetContents()->Clear();
		conn_status_window->GetContents()->TextDraw(0, 0, Font::ColorDefault, s);

		#if defined(INGAME_CHAT)
			Chat_Multiplayer::setStatusConnection(s=="Connected");
		#endif
	}

	void SpawnOtherPlayer(std::string uid) {
		auto& player = Main_Data::game_player;
		auto& nplayer = players[uid].ch;
		nplayer = std::make_shared<Game_PlayerOther>();
		nplayer->SetX(player->GetX());
		nplayer->SetY(player->GetY());
		nplayer->SetSpriteGraphic(player->GetSpriteName(), player->GetSpriteIndex());
		nplayer->SetMoveSpeed(player->GetMoveSpeed());
		nplayer->SetMoveFrequency(player->GetMoveFrequency());
		nplayer->SetThrough(true);
		nplayer->SetLayer(player->GetLayer());

		nameTagRenderer->createNameTag(uid, nplayer.get());

		auto scene_map = Scene::Find(Scene::SceneType::Map);
		if (scene_map == nullptr) {
			Output::Debug("unexpected");
			return;
		}
		auto old_list = &DrawableMgr::GetLocalList();
		DrawableMgr::SetLocalList(&scene_map->GetDrawableList());
		players[uid].sprite = std::make_unique<Sprite_Character>(nplayer.get());
		DrawableMgr::SetLocalList(old_list);
	}
	void SendMainPlayerPos() {
		auto& player = Main_Data::game_player;
		uint16_t cmsg[3] = {
			PacketTypes::movement,
			(uint16_t)player->GetX(),
			(uint16_t)player->GetY()
		};
		TrySend((void*)cmsg, sizeof(uint16_t) * 3);
	}
	
	
	void SendMainPlayerMoveSpeed(int spd) {
		uint16_t m[2] = {PacketTypes::movementAnimationSpeed, (uint16_t)spd};
		TrySend(m, 4);
	}
	

	void SendMainPlayerSprite(std::string name, int index) {

		//sprite packet [uint16_t, uint16_t, null terminated string] (packet type, sprite id, sprite sheet)

		uint16_t msgprefix[2] = {
			PacketTypes::sprite,
			(uint16_t)index,
		};
		size_t s = sizeof(char) * (name.length()) + sizeof(uint16_t) * 2;
		memcpy(sendBuffer, msgprefix, sizeof(uint16_t) * 2);
		memcpy(sendBuffer + sizeof(uint16_t) * 2, name.c_str(), name.length());
		TrySend(sendBuffer, s);
	}

	void SendMainPlayerName() {
		if(multiplayer__my_name.length() == 0) 
			return;
		int s = multiplayer__my_name.length() + sizeof(uint16_t);
		memcpy(sendBuffer, &PacketTypes::name, sizeof(uint16_t));
		memcpy(sendBuffer + sizeof(uint16_t), multiplayer__my_name.c_str(), multiplayer__my_name.length());
		TrySend(sendBuffer, s);
	}

	//this assumes that the player is stopped
	void MovePlayerToPos(std::shared_ptr<Game_PlayerOther> &player, int x, int y) {
		if (!player->IsStopping()) {
			Output::Debug("MovePlayerToPos unexpected error: the player is busy being animated");
		}
		int dx = x - player->GetX();
		int dy = y - player->GetY();
		if (abs(dx) > 1 || abs(dy) > 1 || dx == 0 && dy == 0) {
			player->SetX(x);
			player->SetY(y);
			return;
		}
		int dir[3][3] = {{Game_Character::Direction::UpLeft, Game_Character::Direction::Up, Game_Character::Direction::UpRight},
						 {Game_Character::Direction::Left, 0, Game_Character::Direction::Right},
						 {Game_Character::Direction::DownLeft, Game_Character::Direction::Down, Game_Character::Direction::DownRight}};
		player->Move(dir[dy+1][dx+1]);
	}

	extern "C" {
	void SlashCommandSetSprite(const char* sheet, int id);
	}
	EM_BOOL onopen(int eventType, const EmscriptenWebSocketOpenEvent *websocketEvent, void *userData) {
		SetConnStatusWindowText("Connected");
		//puts("onopen");
		connected = true;
		auto& player = Main_Data::game_player;
		TrySend(Player::emscripten_game_name + "game" + str::to_string(room_id));
		uint16_t room_id16[] = {(uint16_t)room_id};
		TrySend((void*)room_id16, sizeof(uint16_t));
		SendMainPlayerPos();
		if(MultiplayerSettings::spritesheet != "")
			SlashCommandSetSprite(MultiplayerSettings::spritesheet.c_str(), MultiplayerSettings::spriteid);
		SendMainPlayerSprite(player->GetSpriteName(), player->GetSpriteIndex());
		SendMainPlayerName();
		SendMainPlayerMoveSpeed((int)(player->GetMoveSpeed()));
		return EM_TRUE;
	}
	EM_BOOL onclose(int eventType, const EmscriptenWebSocketCloseEvent *websocketEvent, void *userData) {
		SetConnStatusWindowText("Disconnected");
		//puts("onclose");
		connected = false;
		emscripten_websocket_deinitialize();
		ConnectToGame();

		return EM_TRUE;
	}

	void ResolveObjectSyncPacket(const nx_json* json);

	EM_BOOL onmessage(int eventType, const EmscriptenWebSocketMessageEvent *websocketEvent, void *userData) {
		if(websocketEvent->isText) {
			
			strcpy(receiveBuffer, (char*)websocketEvent->data);
			
			const nx_json* json = nx_json_parse(receiveBuffer, NULL);
			if(json) {
				const nx_json* typeNode = nx_json_get(json, "type");
				
				if(typeNode->type == nx_json_type::NX_JSON_STRING) {
					/*if(strcmp(typeNode->text_value, "fyllSync") == 0) {
						const nx_json* syncarray = nx_json_get(json, "data");
						if(syncarray->type == nx_json_type::NX_JSON_ARRAY) {
							for(int i = 0; i < syncarray->children.length; i++) {
								ResolveObjectSyncPacket(nx_json_item(syncarray, i));
							}
						}
					}*/

					if(strcmp(typeNode->text_value, "objectSync") == 0) {
						ResolveObjectSyncPacket(json);
					}
					else
					if(strcmp(typeNode->text_value, "disconnect") == 0) {
						auto scene_map = Scene::Find(Scene::SceneType::Map);
						const nx_json* uid = nx_json_get(json, "uuid");
						auto old_list = &DrawableMgr::GetLocalList();
						DrawableMgr::SetLocalList(&scene_map->GetDrawableList());

						nameTagRenderer->deleteNameTag(uid->text_value);

						players.erase(uid->text_value);
						DrawableMgr::SetLocalList(old_list);
					}
				}
				
			}
			
			nx_json_free(json);
			
		}
		return EM_TRUE;
	}


	void ResolveObjectSyncPacket(const nx_json* json) {
		if(json->type == nx_json_type::NX_JSON_OBJECT) {
			const nx_json* pos = nx_json_get(json, "pos");
			const nx_json* path = nx_json_get(json, "path");;
			const nx_json* sprite = nx_json_get(json, "sprite");
			const nx_json* sound = nx_json_get(json, "sound");
			const nx_json* uid = nx_json_get(json, "uid");
			const nx_json* name = nx_json_get(json, "name");
			const nx_json* weather = nx_json_get(json, "weather");
			const nx_json* mAnimSpd = nx_json_get(json, "movementAnimationSpeed");
			const nx_json* variable = nx_json_get(json, "varialbe");
			const nx_json* switchsync = nx_json_get(json, "switchsync");
			const nx_json* animtype = nx_json_get(json, "animtype");
			const nx_json* animframe = nx_json_get(json, "animframe");
			const nx_json* facing = nx_json_get(json, "facing");
			//const nx_json* typingstatus = nx_json_get(json, "typingstatus");
			
			if(uid->type == nx_json_type::NX_JSON_STRING) {


				std::string uids = std::string(uid->text_value);
				auto player = players.find(uids);
				if(player == players.cend()) {
					SpawnOtherPlayer(std::string(uid->text_value));
					player = players.find(uids);
				}

				MPPlayer& mpplayer = player->second;


				if(pos->type == nx_json_type::NX_JSON_OBJECT) {
					mpplayer.mvq.push(std::make_pair(nx_json_get(pos, "x")->num.u_value, nx_json_get(pos, "y")->num.u_value));
				}
				else if(path->type == nx_json_type::NX_JSON_ARRAY) {
					for(int i = 0; i < path->children.length; i++) {
						pos = nx_json_item(path, i);
						if(pos->type == nx_json_type::NX_JSON_OBJECT) {
							mpplayer.mvq.push(std::make_pair(nx_json_get(pos, "x")->num.u_value, nx_json_get(pos, "y")->num.u_value));
						}
					}
				}

				if(sprite->type == nx_json_type::NX_JSON_OBJECT) {
					const nx_json* sheet = nx_json_get(sprite, "sheet");
					const nx_json* id = nx_json_get(sprite, "id");
					mpplayer.ch->SetSpriteGraphic(std::string(sheet->text_value), id->num.u_value);
					mpplayer.ch->ResetAnimation();
				}

				if(sound->type == nx_json_type::NX_JSON_OBJECT) {
					const nx_json* volume = nx_json_get(sound, "volume");
					const nx_json* tempo = nx_json_get(sound, "tempo");
					const nx_json* balance = nx_json_get(sound, "balance");
					const nx_json* name = nx_json_get(sound, "name");

					lcf::rpg::Sound soundStruct;
					auto& p = mpplayer;
					int dx = p.ch->GetX() - Main_Data::game_player->GetX();
					int dy = p.ch->GetY() - Main_Data::game_player->GetY();
					int distance = std::sqrt(dx * dx + dy * dy);
					soundStruct.volume = std::max(0, 
					(int)
					((100.0f - ((float)distance) * 10.0f) * (float(MultiplayerSettings::playersVolume) / 100.0f) * (float(volume->num.u_value) / 100.0f))
					);
					soundStruct.tempo = tempo->num.u_value;
					soundStruct.balance = balance->num.u_value;
					soundStruct.name = std::string(name->text_value);

					Main_Data::game_system->SePlay(soundStruct);
				}

				if(name->type == nx_json_type::NX_JSON_STRING) {
					nameTagRenderer->setTagName(uids, name->text_value);
				}

				if(weather->type == nx_json_type::NX_JSON_OBJECT) {
					const nx_json* type = nx_json_get(weather, "type");
					const nx_json* strength = nx_json_get(weather, "strength");
					Main_Data::game_screen.get()->SetWeatherEffect(type->num.u_value, strength->num.u_value);
				}

				if(mAnimSpd->type == nx_json_type::NX_JSON_INTEGER) {
					mpplayer.moveSpeed = mAnimSpd->num.u_value;
				}

				if(variable->type == nx_json_type::NX_JSON_OBJECT && false) {
					const nx_json* id = nx_json_get(variable, "id");
					const nx_json* value = nx_json_get(variable, "value");
					Main_Data::game_variables->Set(id->num.u_value, value->num.s_value);
					Game_Map::SetNeedRefresh(true);
					
					std::string setvarstr = std::to_string(id->num.u_value) + " " + std::to_string(value->num.s_value);
					std::string varstr = "var";
					EM_ASM({
						PrintChatInfo(UTF8ToString($0), UTF8ToString($1));
					}, setvarstr.c_str(), varstr.c_str());
				}

				if(switchsync->type == nx_json_type::NX_JSON_OBJECT && MultiplayerSettings::switchsync) {
					const nx_json* id = nx_json_get(switchsync, "id");
					const nx_json* value = nx_json_get(switchsync, "value");
					if(MultiplayerSettings::syncedswitches.find(id->num.u_value) != MultiplayerSettings::syncedswitches.cend()) {
						Main_Data::game_switches->Set(id->num.u_value, value->num.s_value);
						Game_Map::SetNeedRefresh(true);
					}
					std::string setswtstr = std::to_string(id->num.u_value) + " " + std::to_string(value->num.s_value);
					if(MultiplayerSettings::switchlogblacklist.find(id->num.u_value) == MultiplayerSettings::switchlogblacklist.cend()) {
						EM_ASM({
							console.log("switch " + UTF8ToString($0));
						}, setswtstr.c_str());
					}
				}

				if(animtype->type == nx_json_type::NX_JSON_INTEGER) {
					mpplayer.ch->SetAnimationType((lcf::rpg::EventPage::AnimType)animtype->num.u_value);
				}

				if(animframe->type == nx_json_type::NX_JSON_INTEGER) {
					mpplayer.ch->SetAnimFrame(animframe->num.u_value);
				}

				if(facing->type == nx_json_type::NX_JSON_INTEGER) {
					if(facing->num.u_value <= 4)
						mpplayer.ch->SetFacing(facing->num.u_value);
				}
				/*
				if(typingstatus->type == nx_json_type::NX_JSON_INTEGER) {
					mpplayer.typingstatus = typingstatus->num.u_value;
				}*/
			}
		}
	}
}

//this will only be called from outside
extern "C" {

void gotMessage(const char* name, const char* trip, const char* msg, const char* src) {
	#if defined(INGAME_CHAT)
		Chat_Multiplayer::gotMessage(name, trip, msg, src);
	#endif
}

void gotChatInfo(const char* source, const char* text) {
	#if defined(INGAME_CHAT)
		Chat_Multiplayer::gotInfo(text);
	#endif
}

void SendChatMessage(const char* msg) {
	EM_ASM({
		SendMessageString(UTF8ToString($0));
	}, msg);
}

void ChangeName(const char* name) {
	multiplayer__my_name = name;
	SendMainPlayerName();
}

void SlashCommandSetSprite(const char* sheet, int id) {
	MultiplayerSettings::spritesheet = sheet;
	MultiplayerSettings::spriteid = id;
	if(MultiplayerSettings::spritesheet.length())
		Main_Data::game_player->SetSpriteGraphic(sheet, id);
}

void SetPlayersVolume(int volume) {
	MultiplayerSettings::playersVolume = volume;
}

void SetWSHost(const char* host) {
	server_url = host;
}

void SetSwitchSync(int val) {
	MultiplayerSettings::switchsync = val;
}

void SetSwitchSyncWhiteList(int id, int val) {
	if(val) {
		MultiplayerSettings::syncedswitches.emplace(id);
	} else {
		MultiplayerSettings::syncedswitches.erase(id);
	}
}

void SetSwitchSyncLogBlackList(int id, int val) {
	if(val) {
		MultiplayerSettings::switchlogblacklist.emplace(id);
	} else {
		MultiplayerSettings::switchlogblacklist.erase(id);
	}
}

void LogSwitchSyncWhiteList() {
	std::string liststr = "";
	for(auto& swt : MultiplayerSettings::syncedswitches) {
		liststr += std::to_string(swt) + ",";
	}

	EM_ASM({console.log(UTF8ToString($0));}, liststr.c_str());
}
}

void ConnectToGame() {
	EmscriptenWebSocketCreateAttributes ws_attrs = {
		server_url.c_str(),
		"binary",
		EM_TRUE
	};

	socket = emscripten_websocket_new(&ws_attrs);
	emscripten_websocket_set_onopen_callback(socket, NULL, onopen);
	emscripten_websocket_set_onclose_callback(socket, NULL, onclose);
	emscripten_websocket_set_onmessage_callback(socket, NULL, onmessage);
	SetConnStatusWindowText("Disconnected");
}

void Game_Multiplayer::Connect(int map_id) {
	firstRoomUpdate = true;
	room_id = map_id;
	Game_Multiplayer::Quit();
	//if the window doesn't exist (first map loaded) then create it
	//else, if the window is visible recreate it
	if (conn_status_window.get() == nullptr || conn_status_window->IsVisible()) {
		auto scene_map = Scene::Find(Scene::SceneType::Map);
		if (scene_map == nullptr) {
			Output::Debug("unexpected");
		}
		else {
			auto old_list = &DrawableMgr::GetLocalList();
			DrawableMgr::SetLocalList(&scene_map->GetDrawableList());
			conn_status_window = std::make_unique<Window_Base>(0, SCREEN_TARGET_HEIGHT-30, 100, 30);
			conn_status_window->SetContents(Bitmap::Create(100, 30));
			conn_status_window->SetZ(2106632960);
			conn_status_window->SetVisible(false); // TODO: actually remove conn_status_window code.
			DrawableMgr::SetLocalList(old_list);
		}
	}
	
	SetConnStatusWindowText("Disconnected");

	ConnectToGame();
	else {
		uint16_t room_id16[] = {(uint16_t)room_id};
		TrySend((void*)room_id16, sizeof(uint16_t));
		SetConnStatusWindowText("Connected");
	}

	

	EM_ASM({
		ConnectToLocalChat($0);
		SetRoomID($0);
	}, room_id);

	std::string msg = "Connected to room " + std::to_string(room_id);
	std::string source = "Client";
	EM_ASM({
			if(shouldPrintRoomConnetionMessages)
				PrintChatInfo(UTF8ToString($0), UTF8ToString($1));
	}, msg.c_str(), source.c_str());
	
	#if defined(INGAME_CHAT)
		Chat_Multiplayer::setStatusRoom(room_id);
		Chat_Multiplayer::refresh();
	#endif

	// initialize nametag renderer	
	if(nameTagRenderer == nullptr) {
		nameTagRenderer = std::make_unique<DrawableNameTags>();
	}
}

void Game_Multiplayer::Quit() {
	players.clear();
	nameTagRenderer->clearNameTags();
}

void Game_Multiplayer::MainPlayerMoved(int dir) {
	SendMainPlayerPos();
}

void Game_Multiplayer::MainPlayerChangedMoveSpeed(int spd) {
	SendMainPlayerMoveSpeed(spd);
}

void Game_Multiplayer::MainPlayerChangedSpriteGraphic(std::string name, int index) {
	SendMainPlayerSprite(name, index);
}

void Game_Multiplayer::SePlaySync(const lcf::rpg::Sound& sound) {
	if(sound.volume == 0)
		return;
	size_t s = sizeof(uint16_t) * 4 + sound.name.length();
	uint16_t vtd[4] = {PacketTypes::sound, (uint16_t)sound.volume, (uint16_t)sound.tempo, (uint16_t)sound.balance};
	memcpy(sendBuffer, vtd, sizeof(uint16_t) * 4);
	memcpy(sendBuffer + sizeof(uint16_t) * 4, sound.name.c_str(), sound.name.length());
	TrySend(sendBuffer, s);
}

void Game_Multiplayer::WeatherEffectSync(int type, int strength) {
	MultiplayerSettings::nextWeatherType = type;
	MultiplayerSettings::nextWeatherStrength = strength;
}

void Game_Multiplayer::VariableSync(int32_t id, int32_t val) {
	return;
	//uint16_t ptype = PacketTypes::variable;
	//memcpy(sendBuffer, &ptype, sizeof(uint16_t));
	//int32_t m[2] = {id, val};
	//memcpy(sendBuffer + sizeof(uint16_t), m, sizeof(int32_t) * 2);
	//TrySend(&sendBuffer, sizeof(int16_t) * 5);
}

void Game_Multiplayer::SwitchSync(int32_t id, int32_t val) {
	if(MultiplayerSettings::switchsync) {
		if(MultiplayerSettings::syncedswitches.find(id) != MultiplayerSettings::syncedswitches.cend()) {
			uint16_t ptype = PacketTypes::switchsync;
			memcpy(sendBuffer, &ptype, sizeof(uint16_t));
			int32_t m[2] = {id, val};
			memcpy(sendBuffer + sizeof(uint16_t), m, sizeof(int32_t) * 2);
			TrySend(&sendBuffer, sizeof(uint16_t) * 5);
		}
		std::string setswtstr = std::to_string(id) + " " + std::to_string(val);
		if(MultiplayerSettings::switchlogblacklist.find(id) == MultiplayerSettings::switchlogblacklist.cend()) {
			EM_ASM({
				console.log("my switch " + UTF8ToString($0));
			}, setswtstr.c_str());
		}
	}
}

void Game_Multiplayer::AnimTypeSync(lcf::rpg::EventPage::AnimType animtype) {
	uint16_t m[] = {PacketTypes::animtype, (uint16_t)animtype};
	TrySend(m, sizeof(uint16_t) * 2);
}

void Game_Multiplayer::AnimFrameSync(uint16_t frame) {
	uint16_t m[] = {PacketTypes::animframe, (uint16_t)frame};
	TrySend(m, sizeof(uint16_t) * 2);
}

void Game_Multiplayer::FacingSync(uint16_t facing) {
	uint16_t m[] = {PacketTypes::facing, (uint16_t)facing};
	TrySend(m, sizeof(uint16_t) * 2);
}

void Game_Multiplayer::SetTypingStatus(uint16_t status) {
	uint16_t m[] = {PacketTypes::typingstatus, status};
	TrySend(m, sizeof(uint16_t) * 2);
}

void SyncMe() {
	uint16_t m[] = {PacketTypes::syncme, (uint16_t)0};
	TrySend(m, sizeof(uint16_t) * 2);
}

void Game_Multiplayer::FlashAll(int r, int g, int b, int p, int t) {
	for (auto& mpplayer : players) {
		mpplayer.second.ch->Flash(r, g, b, p, t);
	}
}

void Game_Multiplayer::TintAll() {
	for (auto& mpplayer : players) {
		mpplayer.second.sprite->SetTone(Main_Data::game_screen->GetTone());
	}
}

void Game_Multiplayer::Update() {
	if(MultiplayerSettings::nextWeatherType != -1) {
		MultiplayerSettings::weatherT++;
		if(MultiplayerSettings::weatherT > MultiplayerSettings::weatherSetDelay) {
			MultiplayerSettings::weatherT = 0;
			uint16_t msg[3] = {PacketTypes::weather, (uint16_t)MultiplayerSettings::nextWeatherType, (uint16_t)MultiplayerSettings::nextWeatherStrength};
			TrySend(msg, sizeof(uint16_t) * 3);
			MultiplayerSettings::nextWeatherType = -1;
			MultiplayerSettings::nextWeatherStrength = -1;
		}
	}

	for (auto& p : players) {
		auto& q = p.second.mvq;
		if (!q.empty() && p.second.ch->IsStopping()) {
			auto posX = q.front().first;
			auto posY = q.front().second;
			MovePlayerToPos(p.second.ch, posX, posY);
			nameTagRenderer->moveNameTag(p.first, posX, posY);
			if(q.size() > 8) {
				p.second.ch->SetMoveSpeed(6);
				while(q.size() > 16)
						q.pop();
			} else {
				p.second.ch->SetMoveSpeed(p.second.moveSpeed);
			}
			q.pop();
		}
		p.second.ch->SetProcessed(false);
		p.second.ch->Update();
		p.second.sprite->Update();
	}

	if(MultiplayerSettings::shouldsync) {
		SyncMe();
		MultiplayerSettings::shouldsync = false;
	}

	if(!connected) {
		time_t currentTime;
		time(&currentTime);
		
		if(currentTime - MultiplayerSettings::lastConnect < MultiplayerSettings::reconnectInterval) {
			ConnectToGame();
			MultiplayerSettings::lastConnect = currentTime;
		}
	}

	if(firstRoomUpdate) {
		if(MultiplayerSettings::spritesheet != "")
			SlashCommandSetSprite(MultiplayerSettings::spritesheet.c_str(), MultiplayerSettings::spriteid);
		auto& player = Main_Data::game_player;
		SendMainPlayerPos();
		SendMainPlayerSprite(player->GetSpriteName(), player->GetSpriteIndex());
		SendMainPlayerName();
		SendMainPlayerMoveSpeed((int)(player->GetMoveSpeed()));
		
		firstRoomUpdate = false;
	}
}



