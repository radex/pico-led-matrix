#include <Arduino.h>
#include <RP2040_SD.h>
#include <SPI.h>

#define SD_DET_PIN 28

#if PIN_SPI_SS != 17
#error "PIN_SPI_SS must be 17"
#endif
#if PIN_SPI_SCK != 18
#error "PIN_SPI_SCK must be 18"
#endif
#if PIN_SPI_MOSI != 19
#error "PIN_SPI_MOSI must be 19"
#endif
#if PIN_SPI_MISO != 16
#error "PIN_SPI_MISO must be 16"
#endif

void setupSDPins() {
  // TODO: Is that even needed if we use built-in SPI?
  pinMode(PIN_SPI_MISO, INPUT);
  pinMode(PIN_SPI_SS, OUTPUT);
  digitalWrite(PIN_SPI_SS, HIGH);
  pinMode(PIN_SPI_SCK, OUTPUT);
  pinMode(PIN_SPI_MOSI, OUTPUT);
  pinMode(SD_DET_PIN, INPUT_PULLUP);
}

bool isSDCardInserted() {
  return digitalRead(SD_DET_PIN) == LOW;
}

void printSDConfig();
void testSDCard();
void printSDStats();
void printDirectory(File dir, int numTabs);

void setupSD() {
  SPI.begin();

  printSDConfig();

  if (!SD.begin(PIN_SPI_SS)) {
    Serial.println("SD Initialization failed!");
    // Serial.print("Error code: ");
    // Serial.println(SD.card.errorCode(), HEX);
    return;
  }

  Serial.println("SD Initialization done");

  testSDCard();
  // printSDStats(SD.volume);

  File root = SD.open("/");
  printDirectory(root, 0);
}

void printSDConfig() {
  Serial.println(BOARD_NAME);
  Serial.println(RP2040_SD_VERSION);

  Serial.print("Initializing SD card with SS = ");
  Serial.println(PIN_SPI_SS);
  Serial.print("SCK = ");
  Serial.println(PIN_SPI_SCK);
  Serial.print("MOSI = ");
  Serial.println(PIN_SPI_MOSI);
  Serial.print("MISO = ");
  Serial.println(PIN_SPI_MISO);
}

void testSDCard() {
  File myFile = SD.open("test.txt", FILE_WRITE);

  // if the file opened okay, write to it:
  if (myFile) {
    Serial.print("Writing to test.txt...");
    myFile.println("testing 1, 2, 3.");
    myFile.close();
    Serial.println("done.");
  } else {
    Serial.println("error opening test.txt");
  }

  // re-open the file for reading:
  myFile = SD.open("test.txt");
  if (myFile) {
    Serial.println("test.txt:");

    while (myFile.available()) {
      Serial.write(myFile.read());
    }
    myFile.close();
  } else {
    Serial.println("error opening test.txt");
  }
}

void printDirectory(File dir, int numTabs) {
  while (true) {
    File entry = dir.openNextFile();

    if (!entry) {
      // no more files
      break;
    }

    for (uint8_t i = 0; i < numTabs; i++) {
      Serial.print('\t');
    }

    Serial.print(entry.name());

    if (entry.isDirectory()) {
      Serial.println("/");
      printDirectory(entry, numTabs + 1);
    } else {
      // files have sizes, directories do not
      Serial.print("\t\t");
      Serial.println(entry.size(), DEC);
    }
    entry.close();
  }
}

void printSDStats(RP2040_SdVolume volume) {
  Serial.print("Clusters:          ");
  Serial.println(volume.clusterCount());
  Serial.print("Blocks x Cluster:  ");
  Serial.println(volume.blocksPerCluster());

  Serial.print("Total Blocks:      ");
  Serial.println(volume.blocksPerCluster() * volume.clusterCount());
  Serial.println();

  // print the type and size of the first FAT-type volume
  uint32_t volumesize;
  Serial.print("Volume type is:    FAT");
  Serial.println(volume.fatType(), DEC);

  volumesize = volume.blocksPerCluster();    // clusters are collections of blocks
  volumesize *= volume.clusterCount();       // we'll have a lot of clusters
  volumesize /= 2;                           // SD card blocks are always 512 bytes (2 blocks are 1 KB)
  Serial.print("Volume size (KB):  ");
  Serial.println(volumesize);
  Serial.print("Volume size (MB):  ");
  volumesize /= 1024;
  Serial.println(volumesize);
  Serial.print("Volume size (GB):  ");
  Serial.println((float)volumesize / 1024.0);
}
