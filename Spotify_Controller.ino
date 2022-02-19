#include "arduino_secrets.h"
#include <ArduinoJson.h> // needed for better memory management, check out https://arduinojson.org/
#include <ArduinoHttpClient.h>
#include <Arduino_MKRIoTCarrier.h>

#include "thingProperties.h"
#include "spotifyLogo.h"

enum DrawnState { None, Play, Pause };

const char SPOTIFY_CLIENT[] = SECRET_SPOTIFY_CLIENT;  // Client ID of your Spotify app
const char SPOTIFY_SECRET[] = SECRET_SPOTIFY_SECRET;  // Client secret of your Spotify app
const unsigned long TOKEN_REFRESH_RATE = 3000000; // Every 3000 seconds
const unsigned long UPDATE_REFRESH_RATE = 1000; // Every second

WiFiServer server(80); // Server for Spotify authorization
WiFiSSLClient ssl; // Client for Spotify HTTPS requests
HttpClient authClient = HttpClient(ssl, "accounts.spotify.com", 443);
HttpClient apiClient = HttpClient(ssl, "api.spotify.com", 443);
MKRIoTCarrier carrier;

String accessToken;
String refreshToken;
bool authenticated = false;
unsigned long lastTokenTime = 0;
unsigned long lastTrackTime = 0;
int songProgress = 0;
DrawnState lastDrawnState = None;

//############################################ UTILITY ##########################################################
// Creates a filter to only get required parameters
StaticJsonDocument<200> getFilter() {
  StaticJsonDocument<200> filter;

  // Set required data to true
  filter["is_playing"] = true;
  filter["device"]["volume_percent"] = true;
  filter["item"]["name"] = true;
  filter["item"]["album"]["artists"] = true;
  filter["item"]["duration_ms"] = true;
  filter["progress_ms"] = true;

  return filter;
}

// Prints the current song to serial
void printCurrentSong() {
  Serial.print("Now playing: ");
  Serial.print(song_name);
  Serial.print(" by ");
  Serial.println(artist_name);
}

// Returns the style HTML tag
const char* getStyle() {
  return "<style>html{height:100\%;display:grid;justify-content:center;align-content:center;"
         "background-color:#1ED760;font-size:60px;}</style>";
}

// Returns a simple HTML page
String getHTML(const char* message) {
  String html = "<!DOCTYPE html>\n";
  html += "<html><body>";
  html += getStyle();
  html += "<div>";
  html += message;
  html += "</div></body></html>";
  return html;
}

// Returns the index of the last space before a threshold
int lastSpaceBeforeThreshold(String text, int threshold) {
  int prevIndex = 0;
  int index = text.indexOf(' ');
  while (index >= 0 && index < threshold) {
    prevIndex = index;
    index = text.indexOf(' ', index + 1);
  }
  return prevIndex;
}

// Returns true if given bit index is 1
bool isButtonPressed(byte states, byte index) {
  return (states & (1 << index)) > 0;
}
//######################################### AUTHORIZATION #######################################################
// Get the user authorization token
bool getAccessToken(String userCode) {
  String postData = "grant_type=authorization_code&code=" + userCode + "&redirect_uri="
                    "http://" + ip_address + "/redirect/";
  authClient.beginRequest();
  authClient.post("/api/token");
  authClient.sendHeader("Content-Type", "application/x-www-form-urlencoded");
  authClient.sendHeader("Content-Length", postData.length());
  authClient.sendBasicAuth(SPOTIFY_CLIENT, SPOTIFY_SECRET); // send the client id and secret for authentication
  authClient.beginBody();
  authClient.print(postData);
  authClient.endRequest();

  // If successful
  if (authClient.responseStatusCode() == 200) {
    lastTokenTime = millis();
    DynamicJsonDocument json(512);
    deserializeJson(json, authClient.responseBody());
    accessToken = json["access_token"].as<String>();
    refreshToken = json["refresh_token"].as<String>();
    return true;
  }
  return false;
}

// Refresh the user authentication token
void refreshAccessToken() {
  String postData = "grant_type=refresh_token&refresh_token=" + refreshToken;
  authClient.beginRequest();
  authClient.post("/api/token");
  authClient.sendHeader("Content-Type", "application/x-www-form-urlencoded");
  authClient.sendHeader("Content-Length", postData.length());
  authClient.sendBasicAuth(SPOTIFY_CLIENT, SPOTIFY_SECRET); // send the client id and secret for authentication
  authClient.beginBody();
  authClient.print(postData);
  authClient.endRequest();

  // If successful
  if (authClient.responseStatusCode() == 200) {
    lastTokenTime = millis();
    DynamicJsonDocument json(256);
    deserializeJson(json, authClient.responseBody());
    accessToken = json["access_token"].as<String>();
  }
}

//######################################### API REQUESTS ########################################################
// Get the current player state
void getPlayerState() {
  apiClient.beginRequest();
  apiClient.get("/v1/me/player");
  apiClient.sendHeader("Authorization", "Bearer " + accessToken);
  apiClient.endRequest();

  int statusCode = apiClient.responseStatusCode();
  // If successful and playing (we would've gotten status code 204 otherwise)
  if (statusCode == 200) {
    // Create a filter since the response is too large for our Arduino to handle
    StaticJsonDocument<200> filter = getFilter();
    DynamicJsonDocument json(4096);
    deserializeJson(json, apiClient.responseBody(), DeserializationOption::Filter(filter));
    is_playing = json["is_playing"].as<bool>();
    volume_percent = json["device"]["volume_percent"].as<int>();
    // If the song has changed
    if (song_name != json["item"]["name"].as<String>()) {
      songProgress = json["progress_ms"].as<int>();
    }
    song_name = json["item"]["name"].as<String>();
    artist_name = json["item"]["album"]["artists"][0]["name"].as<String>();
    song_length = json["item"]["duration_ms"].as<int>();
    is_active = true;
    printCurrentSong();
  } else {
    is_active = false;
    is_playing = false;
  }
  updatePlayerDisplay();
}

// Skip a song towards a given direction
void skipSong(String direction) {
  apiClient.beginRequest();
  apiClient.post("/v1/me/player/" + direction);
  apiClient.sendHeader("Content-Length", 0);
  apiClient.sendHeader("Authorization", "Bearer " + accessToken);
  apiClient.endRequest();
}

// Skip to the previous song
void previousSong() {
  skipSong("previous");
  getPlayerState();
}

// Skip to the next song
void nextSong() {
  skipSong("next");
  getPlayerState();
}

// Set the player volume
void setVolume(int newVolume) {
  apiClient.beginRequest();
  apiClient.put("/v1/me/player/volume?volume_percent=" + String(newVolume));
  apiClient.sendHeader("Content-Length", 0);
  apiClient.sendHeader("Authorization", "Bearer " + accessToken);
  apiClient.endRequest();
}

// Increase the volume by 5 percent
void volumeUp() {
  int newVolume = volume_percent > 95 ? 100 : volume_percent + 5;
  setVolume(newVolume);
  getPlayerState();
}

// Decrease the volume by 5 percent
void volumeDown() {
  int newVolume = volume_percent < 5 ? 0 : volume_percent - 5;
  setVolume(newVolume);
  getPlayerState();
}

// Toggle play/pause
void playPause() {
  apiClient.beginRequest();
  apiClient.put("/v1/me/player/" + String(is_playing ? "pause" : "play"));
  apiClient.sendHeader("Content-Length", 0);
  apiClient.sendHeader("Authorization", "Bearer " + accessToken);
  apiClient.endRequest();
  getPlayerState();
}

//######################################### LED CONTROL #########################################################
// Flash all LEDs red
void flashLEDs() {
  for (int i = 0; i < 2; i++) {
    carrier.leds.setPixelColor(0, 0, 128, 0);
    carrier.leds.setPixelColor(1, 0, 128, 0);
    carrier.leds.setPixelColor(2, 0, 128, 0);
    carrier.leds.setPixelColor(3, 0, 128, 0);
    carrier.leds.setPixelColor(4, 0, 128, 0);
    carrier.leds.show();
    delay(100);
    turnOffLEDs();
    delay(100);
  }
}

// Cycle all LEDs
void cycleLEDs() {
  carrier.leds.setPixelColor(0, 128, 0, 0);
  carrier.leds.show();
  delay(20);
  carrier.leds.setPixelColor(0, 0);
  carrier.leds.setPixelColor(1, 128, 0, 0);
  carrier.leds.show();
  delay(20);
  carrier.leds.setPixelColor(1, 0);
  carrier.leds.setPixelColor(2, 128, 0, 0);
  carrier.leds.show();
  delay(20);
  carrier.leds.setPixelColor(2, 0);
  carrier.leds.setPixelColor(3, 128, 0, 0);
  carrier.leds.show();
  delay(20);
  carrier.leds.setPixelColor(3, 0);
  carrier.leds.setPixelColor(4, 128, 0, 0);
  carrier.leds.show();
  delay(20);
  carrier.leds.setPixelColor(4, 0);
  carrier.leds.show();
}

// Turn all LEDs off
void turnOffLEDs() {
  carrier.leds.setPixelColor(0, 0);
  carrier.leds.setPixelColor(1, 0);
  carrier.leds.setPixelColor(2, 0);
  carrier.leds.setPixelColor(3, 0);
  carrier.leds.setPixelColor(4, 0);
  carrier.leds.show();
}

//######################################### BUTTON CONTROL ######################################################
// Gets all button states at once
byte getButtonStates() {
  byte states = 0;
  states |= carrier.Buttons.onTouchDown(TOUCH0) << 0;
  states |= carrier.Buttons.onTouchDown(TOUCH1) << 1;
  states |= carrier.Buttons.onTouchDown(TOUCH2) << 2;
  states |= carrier.Buttons.onTouchDown(TOUCH3) << 3;
  states |= carrier.Buttons.onTouchDown(TOUCH4) << 4;
  return states;
}

// Check button states and act accordingly
void checkButtonStates() {
  carrier.Buttons.update();
  byte states = getButtonStates();

  if (!authenticated) {
    if (states > 0) {
      flashLEDs();
    }
  } else if (!is_active) {
    if (states > 0) {
      cycleLEDs();
      getPlayerState();      
    }
  } else {
    // Button 0 - Volume Down
    if (isButtonPressed(states, 0)) {
      carrier.leds.setPixelColor(0, 128, 0, 0);
      carrier.leds.show();
      volumeDown();
      carrier.leds.setPixelColor(0, 0);
      carrier.leds.show();
      return;
    }

    // Button 1 - Previous Song
    if (isButtonPressed(states, 1)) {
      carrier.leds.setPixelColor(1, 128, 0, 0);
      carrier.leds.show();
      previousSong();
      carrier.leds.setPixelColor(1, 0);
      carrier.leds.show();
      return;
    }

    // Button 2 - Play/Pause
    if (isButtonPressed(states, 2)) {
      carrier.leds.setPixelColor(2, 128, 0, 0);
      carrier.leds.show();
      playPause();
      carrier.leds.setPixelColor(2, 0);
      carrier.leds.show();
      return;
    }

    // Button 3 - Next Song
    if (isButtonPressed(states, 3)) {
      carrier.leds.setPixelColor(3, 128, 0, 0);
      carrier.leds.show();
      nextSong();
      carrier.leds.setPixelColor(3, 0);
      carrier.leds.show();
      return;
    }

    // Button 4 - Volume Up
    if (isButtonPressed(states, 4)) {
      carrier.leds.setPixelColor(4, 128, 0, 0);
      carrier.leds.show();
      volumeUp();
      carrier.leds.setPixelColor(4, 0);
      carrier.leds.show();
      return;
    }
  }
}

//######################################### DISPLAY CONTROL #####################################################
// Print media icons
void printMediaIcons() {
  // Skip to Previous
  carrier.display.fillTriangle(38, 51, 51, 43, 51, 59, ST77XX_WHITE);
  carrier.display.fillTriangle(51, 51, 64, 43, 64, 59, ST77XX_WHITE);

  // Skip to Next
  carrier.display.fillTriangle(176, 43, 176, 59, 189, 51, ST77XX_WHITE);
  carrier.display.fillTriangle(189, 43, 189, 59, 202, 51, ST77XX_WHITE);

  // Volume Up
  carrier.display.fillRect(203, 154, 8, 24, ST77XX_WHITE);
  carrier.display.fillRect(195, 162, 24, 8, ST77XX_WHITE);

  // Volume Down
  carrier.display.fillRect(21, 162, 24, 8, ST77XX_WHITE);

  // Play/Pause
  printPlayPause();
}

// Print the Spotify logo
void printSpotifyLogo() {
  carrier.display.drawBitmap(60, 60, SPOTIFY_LOGO, 120, 120, 0x26CC);
}

// Initialize player display
void initializePlayerDisplay() {
  carrier.display.fillScreen(ST77XX_BLACK);
  printSpotifyLogo();
  printMediaIcons();
}

// Draw play or pause icon depending on is_playing
void printPlayPause() {
  if (lastDrawnState == (is_playing ? Pause : Play)) {
    return;
  }

  // First clear the location
  carrier.display.fillRect(105, 1, 30, 30, ST77XX_BLACK);

  // Then, draw the corresponding logo
  if (is_playing) {
    // Pause logo
    carrier.display.fillRect(113, 6, 5, 23, ST77XX_WHITE);
    carrier.display.fillRect(122, 6, 5, 23, ST77XX_WHITE);
  } else {
    // Play logo
    carrier.display.fillTriangle(112, 4, 112, 26, 128, 15, ST77XX_WHITE);
  }
  lastDrawnState = is_playing ? Pause : Play;
}

// Extends the progress bar if the song is playing, else deletes it
void updateTrackBar() {
  if (is_playing) {
    songProgress += millis() - lastTrackTime;
    if (songProgress > song_length) {
      getPlayerState();
      return;
    }
    carrier.display.drawRect(80, 220, 80, 5, ST77XX_WHITE); // draw track bar
    int progress = 76 * songProgress / song_length;
    carrier.display.fillRect(82, 221, progress, 3, ST77XX_WHITE); // draw progress bar
    carrier.display.fillRect(82 + progress, 221, 76 - progress, 3, ST77XX_BLACK); // draw non progressed part
  } else {
    carrier.display.fillRect(80, 220, 80, 5, ST77XX_BLACK); // delete track bar
  }
  lastTrackTime = millis();
}

// Updates the track name if the song is playing, else deletes it
void updateTrackName() {
  carrier.display.fillRect(70, 186, 100, 33, ST77XX_BLACK); // clear track name
  if (is_playing) {
    // Print the song name
    carrier.display.setTextColor(ST77XX_WHITE);
    carrier.display.setTextSize(1); // 6 x 8
    carrier.display.setCursor(70, 186);
    if (song_name.length() > 16) {
      // Try not to break words in half
      int lastSpace = lastSpaceBeforeThreshold(song_name, 16);
      if (lastSpace > 0) {
        carrier.display.print(song_name.substring(0, lastSpace));
        carrier.display.setCursor(70, 194);
        carrier.display.print(song_name.substring(lastSpace + 1, song_name.length() > lastSpace + 17 ?
                              lastSpace + 17 : song_name.length()));
      } else {
        carrier.display.print(song_name.substring(0, 16));
        carrier.display.setCursor(70, 194);
        carrier.display.print(song_name.substring(16, song_name.length() > 32 ? 32 : song_name.length()));
      }
    } else {
      carrier.display.print(song_name);
    }

    // Print the artist name
    carrier.display.setTextColor(0xCE59); // gray
    carrier.display.setTextSize(1); // 6 x 8
    carrier.display.setCursor(70, 209);
    carrier.display.print(artist_name.substring(0, artist_name.length() > 16 ? 16 : artist_name.length()));
  }
}

// Update the state of the player
void updatePlayerDisplay() {
  printPlayPause();
  updateTrackBar();
  updateTrackName();
}

// Print a message to prompt authentication
void printAuthMessage() {
  carrier.display.fillScreen(0x26CC);
  carrier.display.setTextColor(ST77XX_WHITE);
  carrier.display.setTextSize(2); // 12 x 16

  carrier.display.setCursor(80, 92);
  carrier.display.print("Please");

  carrier.display.setCursor(30, 110);
  carrier.display.print("authenticate at");

  carrier.display.setCursor(30 + (22 - ip_address.length()) / 2, 128);
  carrier.display.print(ip_address);
}

//############################################ EVENTS ###########################################################
// Update IP address when connected
void onNetworkConnect() {
  IPAddress ip = WiFi.localIP();
  ip_address = String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
  printAuthMessage();
  Serial.println("Please authenticate at " + ip_address);
}

//######################################### SETUP & LOOP ########################################################
void setup() {
  Serial.begin(9600);

  // Initialize variables
  volume_percent = 0;
  song_length = 0;
  is_active = false;
  is_playing = false;
  song_name = "";
  artist_name = "";

  // Defined in thingProperties.h
  initProperties();

  // Connect to Arduino IoT Cloud
  ArduinoCloud.begin(ArduinoIoTPreferredConnection);

  // Add connection callback
  ArduinoIoTPreferredConnection.addCallback(NetworkConnectionEvent::CONNECTED, onNetworkConnect);

  // Initialize carrier
  CARRIER_CASE = true; // if you don't set it to true, your config will get overwritten
  carrier.Buttons.updateConfig(40); // for all buttons
  carrier.Buttons.updateConfig(66, TOUCH3); // for button 3, seems more sensitive than others
  carrier.begin();
  carrier.display.setRotation(0);

  // Begin the server
  server.begin();
}

void loop() {
  ArduinoCloud.update();
  checkButtonStates();

  if (!authenticated) {
    // Wait for an user to connect
    WiFiClient wifiClient = server.available();

    // If a user has connected
    if (wifiClient) {
      // Get the request
      String header = "";
      while (wifiClient.available()) {
        header += char(wifiClient.read());
      }

      // If authenticated, redirect
      if (header.indexOf("?code") >= 0) {
        // Parse the token
        String userCode = header.substring(header.indexOf("code=") + 5, header.indexOf("HTTP/1.1") - 1);
        authenticated = getAccessToken(userCode);
        if (authenticated) {
          wifiClient.print(getHTML("Authenticated!"));
          initializePlayerDisplay();
          getPlayerState();
        } else {
          wifiClient.print(getHTML("Authentication failed."));
        }
      } else if (header.indexOf("?error") >= 0) {
        wifiClient.print(getHTML("Cancelled."));
      }
      else {
        // Authenticate the user and get the code
        String webpage = "<!DOCTYPE html>\n";
        webpage += "<html><body>";
        webpage += getStyle();
        webpage += "<a href=\"https://accounts.spotify.com/authorize?client_id=";
        webpage += SPOTIFY_CLIENT;
        webpage += "&response_type=code&redirect_uri=http://";
        webpage += ip_address;
        webpage += "/redirect/&scope=user-read-playback-state "
                   "user-modify-playback-state\">Authenticate Spotify</a>\n";
        webpage += "</body></html>";
        wifiClient.print(webpage);
      }
      wifiClient.stop();
    }
  } else if (millis() - lastTokenTime > TOKEN_REFRESH_RATE) {
    // Refresh token if it is about to expire
    refreshAccessToken();
  } else if (millis() - lastTrackTime > UPDATE_REFRESH_RATE) {
    // Update the track bar
    updateTrackBar();
  }
}