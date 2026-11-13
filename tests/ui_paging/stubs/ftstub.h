#pragma once
typedef void* FT_Library; typedef void* FT_Face;
int FT_Init_FreeType(FT_Library*); int FT_Done_FreeType(FT_Library); int FT_New_Face(FT_Library,const char*,long,FT_Face*); int FT_Done_Face(FT_Face);
unsigned int FT_Get_Char_Index(FT_Face, unsigned long);
