#pragma once
#include <string>
#include <vector>
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
namespace miyoofin {
inline constexpr int FB_W=640, FB_H=480, BOTTOM_H=18, POSTER_X=28, POSTER_Y=48, POSTER_W=160, POSTER_H=240, RIGHT_X=215, RIGHT_TOP_Y=48, META_WRAP=34, BTN_W=80, BTN_H=20, BTN_Y=436, BTN_PLAY_X=260, BTN_DL_X=360;
inline constexpr unsigned char FOCUS_OR=255,FOCUS_OG=220,FOCUS_OB=40,FOCUS_IR=255,FOCUS_IG=255,FOCUS_IB=120;
inline std::vector<std::string> wrapText(const char *text,int wrapCols){std::vector<std::string> lines;if(!text||!*text)return lines;std::string input(text);size_t pos=0;while(pos<input.size()){size_t newline=input.find('\n',pos);std::string para=newline!=std::string::npos?input.substr(pos,newline-pos):input.substr(pos);while(!para.empty()){if((int)para.size()<=wrapCols){lines.push_back(para);break;}size_t lastSpace=para.rfind(' ',wrapCols);if(lastSpace!=std::string::npos&&lastSpace>0){lines.push_back(para.substr(0,lastSpace));para=para.substr(lastSpace+1);}else{lines.push_back(para.substr(0,wrapCols));para=para.substr(wrapCols);}}if(newline!=std::string::npos)pos=newline+1;else break;}return lines;}
inline void renderBottomHints(SDL_Surface *fb,const char *hint){int y=FB_H-BOTTOM_H;BitmapFont::fillRect(fb,0,y,FB_W,BOTTOM_H,Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3,255);BitmapFont::drawString(fb,8,y+2,hint,Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3);}
}
