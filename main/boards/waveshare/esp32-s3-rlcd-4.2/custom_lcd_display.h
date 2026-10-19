#ifndef __CUSTOM_LCD_DISPLAY_H__
#define __CUSTOM_LCD_DISPLAY_H__

#include <driver/gpio.h>
#include "lcd_display.h"

enum ColorSelection {
    ColorBlack = 0,    
    ColorWhite = 0xff
};

typedef struct {
    uint8_t mosi;
    uint8_t scl;
    uint8_t dc;
    uint8_t cs;
    uint8_t rst;
} spi_display_config_t;

class CustomLcdDisplay : public LcdDisplay {
private:
    esp_lcd_panel_io_handle_t io_handle = NULL;
    uint32_t            i2c_data_pdMS_TICKS = 0;
    uint32_t            i2c_done_pdMS_TICKS = 0;
    const char         *TAG                 = "CustomDisplay";
    int                 mosi_;
    int                 scl_;
    int                 dc_;
    int                 cs_;
    int                 rst_;
    int                 width_;
    int                 height_;
    uint8_t            *DispBuffer = NULL;
    int                 DisplayLen;
	uint16_t (*PixelIndexLUT)[300];
	uint8_t  (*PixelBitLUT  )[300];
	void InitPortraitLUT();
	void InitLandscapeLUT();
    void Set_ResetIOLevel(uint8_t level);
    void RLCD_SendCommand(uint8_t Reg);
    void RLCD_SendData(uint8_t Data);
    void RLCD_Sendbuffera(uint8_t *Data, int len);
    void RLCD_Reset(void);
    static void Lvgl_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p);

    lv_obj_t* dashboard_root_ = nullptr;
    lv_obj_t* module_grid_ = nullptr;
    lv_obj_t* clock_card_ = nullptr;
    lv_obj_t* clock_time_label_ = nullptr;
    lv_obj_t* clock_date_label_ = nullptr;
    lv_obj_t* agenda_card_ = nullptr;
    lv_obj_t* agenda_label_ = nullptr;
    lv_obj_t* drawing_card_ = nullptr;
    lv_obj_t* drawing_placeholder_label_ = nullptr;
    lv_obj_t* tool_card_ = nullptr;
    lv_obj_t* tool_status_label_ = nullptr;
    lv_obj_t* device_status_card_ = nullptr;
    lv_obj_t* device_status_label_ = nullptr;
    lv_obj_t* dashboard_message_label_ = nullptr;
    lv_obj_t* avatar_image_ = nullptr;

    lv_obj_t* CreateDashboardCard(lv_obj_t* parent, const char* title);
    lv_obj_t* CreateCardLabel(lv_obj_t* parent, const char* text);

public:
    virtual void SetupUI() override;
    virtual void SetStatus(const char* status) override;
    virtual void SetToolStatus(const char* status) override;
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void ClearChatMessages() override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    virtual void SetTheme(Theme* theme) override;

    void SetDashboardClock(const char* time_text, const char* date_text);
    void SetDashboardAgenda(const char* text);
    void SetDashboardToolStatus(const char* text);
    void SetDashboardDrawing(const uint8_t* bitmap, int width, int height);
    void SetDashboardMessage(const char* text);

    CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy,spi_display_config_t spiconfig,spi_host_device_t spi_host = SPI3_HOST);
    ~CustomLcdDisplay();
    void RLCD_Init();
    void RLCD_ColorClear(uint8_t color);
    void RLCD_Display();
	void RLCD_SetPixel(uint16_t x, uint16_t y, uint8_t color);
};

#endif