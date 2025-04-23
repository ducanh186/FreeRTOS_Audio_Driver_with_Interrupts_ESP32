#ifndef _MANAGE_SD_H_
#define _MANAGE_SD_H_

#include <string>
#include "SDCard.h"
#include "esp_vfs_fat.h"

class ManageSD {
private:
  SDCard *m_sdCard;  
  FILE *m_file;     
  char m_fileName[256];  

public:
  ManageSD(SDCard *sdCard);
  ~ManageSD();

  // File Management Methods
  bool listFiles(const char *dirName);  
  bool readCurrentFile(char *buffer, size_t bufferSize); 
  bool openFile(const char *filename);  
  bool closeFile();  
};

#endif
