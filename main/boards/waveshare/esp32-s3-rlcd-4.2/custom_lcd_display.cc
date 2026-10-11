#include <ctime>
#include <vector>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <esp_lcd_panel_io.h>
#include <esp_log.h>
#include <esp_err.h>
#include "custom_lcd_display.h"
#include "lcd_display.h"
#include "lvgl_theme.h"
#include "esp_lvgl_port.h"
#include "material_symbols.h"
#include "assets/lang_config.h"
#include "settings.h"
#include "config.h"
#include "board.h"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);

void CustomLcdDisplay::Lvgl_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p)
{
    assert(disp != NULL);
    CustomLcdDisplay *Disp = (CustomLcdDisplay *)lv_display_get_user_data(disp);
    uint16_t *buffer = (uint16_t *)color_p;
  	for(int y = area->y1; y <= area->y2; y++)
  	{
  	 	for(int x = area->x1; x <= area->x2; x++) 
  	 	{
  	 	   	uint8_t color = (*buffer < 0x7fff) ? ColorBlack : ColorWhite;
  	 	   	Disp->RLCD_SetPixel(x,y,color);
  	 	   	buffer++;
  	 	}
  	}
  	Disp->RLCD_Display();
	lv_disp_flush_ready(disp);
}

CustomLcdDisplay::CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io,
esp_lcd_panel_handle_t panel,
int width, 
int height, 
int offset_x, 
int offset_y,
bool mirror_x, 
bool mirror_y, 
bool swap_xy,
spi_display_config_t spiconfig,
spi_host_device_t spi_host) : LcdDisplay(panel_io, panel, width, height),
mosi_(spiconfig.mosi),
scl_(spiconfig.scl), 
dc_(spiconfig.dc), 
cs_(spiconfig.cs), 
rst_(spiconfig.rst), 
width_(width), 
height_(height)
{
	ESP_LOGI(TAG, "Initialize SPI");
	esp_err_t        ret;
    spi_bus_config_t buscfg   = {};
    int              transfer = width_ * height_;
    buscfg.miso_io_num                   = -1;
    buscfg.mosi_io_num                   = mosi_;
    buscfg.sclk_io_num                   = scl_;
    buscfg.quadwp_io_num                 = -1;
    buscfg.quadhd_io_num                 = -1;
    buscfg.max_transfer_sz               = transfer;
    ret                                  = spi_bus_initialize(spi_host, &buscfg, SPI_DMA_CH_AUTO);
    ESP_ERROR_CHECK(ret);
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.dc_gpio_num = static_cast<gpio_num_t>(dc_);
    io_config.cs_gpio_num = static_cast<gpio_num_t>(cs_);
    io_config.pclk_hz = 40 * 1000 * 1000;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.spi_mode = 0;
    io_config.trans_queue_depth = 1;
    io_config.flags.psram_dma_direct = 1;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)spi_host, &io_config, &io_handle));
    gpio_config_t gpio_conf = {};
    gpio_conf.intr_type     = GPIO_INTR_DISABLE;
    gpio_conf.mode          = GPIO_MODE_OUTPUT;
    gpio_conf.pin_bit_mask  = (0x1ULL << rst_);
    gpio_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    gpio_conf.pull_up_en    = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));
    Set_ResetIOLevel(1);

    DisplayLen                = transfer >> 3; //(1byte 8ipex)
    DispBuffer                = (uint8_t *) heap_caps_malloc(DisplayLen, MALLOC_CAP_SPIRAM);
    assert(DispBuffer);
	PixelIndexLUT = (uint16_t (*)[300])heap_caps_malloc(transfer * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
	PixelBitLUT   = (uint8_t (*)[300])heap_caps_malloc(transfer * sizeof(uint8_t), MALLOC_CAP_SPIRAM);
    assert(PixelIndexLUT);
    assert(PixelBitLUT);
    if(width_ == 400) {
        InitLandscapeLUT();
    } else {
        InitPortraitLUT();
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority   = 2;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);
    lvgl_port_lock(0);

    display_ = lv_display_create(width, height); /* 以水平和垂直分辨率（像素）进行基本初始化 */
    lv_display_set_flush_cb(display_, Lvgl_flush_cb);
    lv_display_set_user_data(display_, this);
	size_t lvgl_buffer_size = LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565) * transfer;
	uint8_t *lvgl_buffer1 = (uint8_t *) heap_caps_malloc(lvgl_buffer_size, MALLOC_CAP_SPIRAM);
    assert(lvgl_buffer1);
	lv_display_set_buffers(display_, lvgl_buffer1, NULL, lvgl_buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    ESP_LOGI(TAG, "RLCD init");
    RLCD_Init();

    lvgl_port_unlock();
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    // Note: SetupUI() should be called by Application::Initialize(), not in constructor
    // to ensure lvgl objects are created after the display is fully initialized.
}

CustomLcdDisplay::~CustomLcdDisplay() {
}

void CustomLcdDisplay::InitPortraitLUT() {
    uint16_t W4 = width_ >> 2;
    for (uint16_t y = 0; y < height_; y++)
    {
        uint16_t byte_y = y >> 1;
        uint8_t  local_y = y & 1;
        for (uint16_t x = 0; x < width_; x++)
        {
            uint16_t byte_x = x >> 2;
            uint8_t  local_x = x & 3;

            uint32_t index = byte_y * W4 + byte_x;
            uint8_t bit = 7 - ((local_x << 1) | local_y);

            PixelIndexLUT[x][y] = index;
            PixelBitLUT  [x][y] = (1 << bit);
        }
    }
}

void CustomLcdDisplay::InitLandscapeLUT() {
    uint16_t H4 = height_ >> 2;
    for (uint16_t y = 0; y < height_; y++)
    {
        uint16_t inv_y = height_ - 1 - y;
        uint16_t block_y = inv_y >> 2;
        uint8_t  local_y  = inv_y & 3;
        for (uint16_t x = 0; x < width_; x++)
        {
            uint16_t byte_x = x >> 1;
            uint8_t  local_x = x & 1;

            uint32_t index = byte_x * H4 + block_y;
            uint8_t bit = 7 - ((local_y << 1) | local_x);

            PixelIndexLUT[x][y] = index;
            PixelBitLUT  [x][y] = (1 << bit);
        }
    }
}

void CustomLcdDisplay::Set_ResetIOLevel(uint8_t level) {
    gpio_set_level((gpio_num_t) rst_, level ? 1 : 0);
}

void CustomLcdDisplay::RLCD_SendCommand(uint8_t Reg) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, Reg, NULL, 0));
}

void CustomLcdDisplay::RLCD_SendData(uint8_t Data) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, -1, &Data, 1));
}

void CustomLcdDisplay::RLCD_Sendbuffera(uint8_t *Data, int len) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(io_handle, -1, Data, len));
}

void CustomLcdDisplay::RLCD_Reset(void) {
    Set_ResetIOLevel(1);
    vTaskDelay(pdMS_TO_TICKS(50));
    Set_ResetIOLevel(0);
    vTaskDelay(pdMS_TO_TICKS(20));
    Set_ResetIOLevel(1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

void CustomLcdDisplay::RLCD_ColorClear(uint8_t color) {
    memset(DispBuffer, color, DisplayLen);
}

void CustomLcdDisplay::RLCD_Init() {
    RLCD_Reset();

    RLCD_SendCommand(0xD6);  // NVM Load Control
	RLCD_SendData(0x17);
	RLCD_SendData(0x02);

	RLCD_SendCommand(0xD1); //Booster Enable
	RLCD_SendData(0x01);

	RLCD_SendCommand(0xC0); //Gate Voltage Control
	RLCD_SendData(0x11);   
	RLCD_SendData(0x04);   

	RLCD_SendCommand(0xC1); //VSHP Setting
	RLCD_SendData(0x69);
	RLCD_SendData(0x69);
	RLCD_SendData(0x69);
	RLCD_SendData(0x69);

	RLCD_SendCommand(0xC2);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);

	RLCD_SendCommand(0xC4);
	RLCD_SendData(0x4B);
	RLCD_SendData(0x4B);
	RLCD_SendData(0x4B);
	RLCD_SendData(0x4B);

	RLCD_SendCommand(0xC5);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);

	RLCD_SendCommand(0xD8);
	RLCD_SendData(0x80);
	RLCD_SendData(0xE9);

	RLCD_SendCommand(0xB2);
	RLCD_SendData(0x02);

	RLCD_SendCommand(0xB3);
	RLCD_SendData(0xE5);
	RLCD_SendData(0xF6);
	RLCD_SendData(0x05);
	RLCD_SendData(0x46);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x76);
	RLCD_SendData(0x45);

	RLCD_SendCommand(0xB4);
	RLCD_SendData(0x05);
	RLCD_SendData(0x46);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x76);
	RLCD_SendData(0x45);

	RLCD_SendCommand(0x62);
	RLCD_SendData(0x32);
	RLCD_SendData(0x03);
	RLCD_SendData(0x1F);

	RLCD_SendCommand(0xB7);
	RLCD_SendData(0x13);

	RLCD_SendCommand(0xB0);
	RLCD_SendData(0x64);

	RLCD_SendCommand(0x11); 
	vTaskDelay(pdMS_TO_TICKS(200));     
	RLCD_SendCommand(0xC9);
	RLCD_SendData(0x00);

	RLCD_SendCommand(0x36);
	RLCD_SendData(0x48); 

	RLCD_SendCommand(0x3A);
	RLCD_SendData(0x11); 

	RLCD_SendCommand(0xB9);
	RLCD_SendData(0x20);

	RLCD_SendCommand(0xB8);
	RLCD_SendData(0x29);

	RLCD_SendCommand(0x21);

	RLCD_SendCommand(0x2A); 
	RLCD_SendData(0x12);
	RLCD_SendData(0x2A);

	RLCD_SendCommand(0x2B); 
	RLCD_SendData(0x00);
	RLCD_SendData(0xC7);

	RLCD_SendCommand(0x35);
	RLCD_SendData(0x00);

	RLCD_SendCommand(0xD0);
	RLCD_SendData(0xFF);

	RLCD_SendCommand(0x38);
	RLCD_SendCommand(0x29);

    RLCD_ColorClear(ColorWhite);
}

void CustomLcdDisplay::RLCD_SetPixel(uint16_t x, uint16_t y, uint8_t color) {
    uint32_t idx = PixelIndexLUT[x][y];
    uint8_t  mask = PixelBitLUT[x][y];

    uint8_t *p = &DispBuffer[idx];

    if (color)
        *p |= mask;
    else
        *p &= ~mask;
}

void CustomLcdDisplay::RLCD_Display() {
    RLCD_SendCommand(0x2A);     // Column Address Set
  	RLCD_SendData(0x12);
  	RLCD_SendData(0x2A);

  	RLCD_SendCommand(0x2B);     // Page Address Set
  	RLCD_SendData(0x00);
  	RLCD_SendData(0xC7);

  	RLCD_SendCommand(0x2c);     // Page Address Set

	RLCD_Sendbuffera(DispBuffer,DisplayLen);
}

lv_obj_t* CustomLcdDisplay::CreateDashboardCard(lv_obj_t* parent, const char* title) {
    auto* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    const lv_font_t* title_font = lvgl_theme != nullptr ? lvgl_theme->text_font()->font()
                                                         : &BUILTIN_TEXT_FONT;
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_pad_all(card, 2, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title_label = lv_label_create(card);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, title_font, 0);
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(title_label, lv_color_black(), 0);
    lv_obj_set_style_radius(title_label, 0, 0);
    lv_obj_set_style_pad_hor(title_label, 6, 0);
    lv_obj_set_style_pad_ver(title_label, 0, 0);

    return card;
}

lv_obj_t* CustomLcdDisplay::CreateCardLabel(lv_obj_t* parent, const char* text) {
    auto* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    const lv_font_t* text_font =
        lvgl_theme != nullptr ? lvgl_theme->text_font()->font() : &BUILTIN_TEXT_FONT;
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, text_font, 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_height(label, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(label, 1, 0);
    return label;
}

void CustomLcdDisplay::SetupUI() {
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    Display::SetupUI();
    DisplayLockGuard lock(this);

    auto* screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    auto* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    const lv_font_t* text_font =
        lvgl_theme != nullptr ? lvgl_theme->text_font()->font() : &BUILTIN_TEXT_FONT;
    const lv_font_t* icon_font =
        lvgl_theme != nullptr ? lvgl_theme->icon_font()->font() : &BUILTIN_ICON_FONT;
    const lv_font_t* large_icon_font =
        lvgl_theme != nullptr && lvgl_theme->large_icon_font() != nullptr
            ? lvgl_theme->large_icon_font()->font()
            : icon_font;
    const lv_font_t* status_icon_font =
        text_font != nullptr && text_font->line_height >= 40 ? large_icon_font : icon_font;

    dashboard_root_ = lv_obj_create(screen);
    lv_obj_set_size(dashboard_root_, LV_HOR_RES, LV_VER_RES);
    lv_obj_remove_flag(dashboard_root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(dashboard_root_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(dashboard_root_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dashboard_root_, 0, 0);
    lv_obj_set_style_radius(dashboard_root_, 0, 0);
    lv_obj_set_style_pad_all(dashboard_root_, 0, 0);
    lv_obj_set_style_layout(dashboard_root_, LV_LAYOUT_NONE, 0);

    // Top status strip: WiFi, mute, battery and board state.
    top_bar_ = lv_obj_create(dashboard_root_);
    lv_obj_set_size(top_bar_, LV_HOR_RES - 16, 28);
    lv_obj_set_pos(top_bar_, 8, 8);
    lv_obj_set_style_bg_color(top_bar_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_pad_hor(top_bar_, 8, 0);
    lv_obj_set_style_pad_ver(top_bar_, 2, 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, status_icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lv_color_black(), 0);

    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_radius(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, status_icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lv_color_black(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, status_icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lv_color_black(), 0);
    lv_obj_set_style_margin_left(battery_label_, 6, 0);

    // Secondary status line used for notifications / status text.
    status_bar_ = lv_obj_create(dashboard_root_);
    lv_obj_set_size(status_bar_, LV_HOR_RES - 16, 22);
    lv_obj_set_pos(status_bar_, 8, 40);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES - 16);
    lv_label_set_long_mode(notification_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(notification_label_, text_font, 0);
    lv_obj_set_style_text_color(notification_label_, lv_color_black(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES - 16);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(status_label_, text_font, 0);
    lv_obj_set_style_text_color(status_label_, lv_color_black(), 0);
    lv_label_set_text(status_label_, "Ready");
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    // Left panel: clock / date / agenda / tool status.
    lv_obj_t* left_panel = lv_obj_create(dashboard_root_);
    lv_obj_set_size(left_panel, 188, LV_VER_RES - 70);
    lv_obj_set_pos(left_panel, 8, 66);
    lv_obj_set_style_bg_color(left_panel, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(left_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(left_panel, 2, 0);
    lv_obj_set_style_border_color(left_panel, lv_color_black(), 0);
    lv_obj_set_style_radius(left_panel, 0, 0);
    lv_obj_set_style_pad_all(left_panel, 8, 0);
    lv_obj_set_style_layout(left_panel, LV_LAYOUT_NONE, 0);

    clock_time_label_ = lv_label_create(left_panel);
    lv_obj_set_width(clock_time_label_, 156);
    lv_obj_set_height(clock_time_label_, 40);
    lv_obj_align(clock_time_label_, LV_ALIGN_TOP_MID, 0, 14);
    lv_label_set_long_mode(clock_time_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(clock_time_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(clock_time_label_, text_font, 0);
    lv_obj_set_style_text_color(clock_time_label_, lv_color_black(), 0);
    lv_label_set_text(clock_time_label_, "--:--");

    clock_date_label_ = lv_label_create(left_panel);
    lv_obj_set_width(clock_date_label_, 156);
    lv_obj_set_height(clock_date_label_, 22);
    lv_obj_align(clock_date_label_, LV_ALIGN_TOP_MID, 0, 56);
    lv_label_set_long_mode(clock_date_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(clock_date_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(clock_date_label_, text_font, 0);
    lv_obj_set_style_text_color(clock_date_label_, lv_color_black(), 0);
    lv_label_set_text(clock_date_label_, "----/--/--");

    agenda_card_ = CreateDashboardCard(left_panel, "Agenda");
    lv_obj_set_size(agenda_card_, 148, 58);
    lv_obj_align(agenda_card_, LV_ALIGN_TOP_MID, 0, 84);
    agenda_label_ = CreateCardLabel(agenda_card_, "No agenda");

    tool_card_ = CreateDashboardCard(left_panel, "Tools");
    lv_obj_set_size(tool_card_, 148, 58);
    lv_obj_align(tool_card_, LV_ALIGN_TOP_MID, 0, 148);
    tool_status_label_ = CreateCardLabel(tool_card_, "Idle");

    // Right panel: Q-style avatar on top, dialogue strip at bottom.
    lv_obj_t* right_panel = lv_obj_create(dashboard_root_);
    lv_obj_set_size(right_panel, 188, LV_VER_RES - 70);
    lv_obj_set_pos(right_panel, 204, 66);
    lv_obj_set_style_bg_color(right_panel, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(right_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(right_panel, 2, 0);
    lv_obj_set_style_border_color(right_panel, lv_color_black(), 0);
    lv_obj_set_style_radius(right_panel, 0, 0);
    lv_obj_set_style_pad_all(right_panel, 8, 0);
    lv_obj_set_style_layout(right_panel, LV_LAYOUT_NONE, 0);

    lv_obj_t* avatar_box = lv_obj_create(right_panel);
    lv_obj_set_size(avatar_box, 164, 156);
    lv_obj_align(avatar_box, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_bg_color(avatar_box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(avatar_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(avatar_box, 1, 0);
    lv_obj_set_style_border_color(avatar_box, lv_color_black(), 0);
    lv_obj_set_style_radius(avatar_box, 0, 0);

    avatar_label_ = lv_label_create(avatar_box);
    lv_obj_set_width(avatar_label_, 156);
    lv_label_set_long_mode(avatar_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(avatar_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(avatar_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(avatar_label_, lv_color_black(), 0);
    lv_label_set_text(avatar_label_, MATERIAL_SYMBOLS_ROBOT_2);
    lv_obj_center(avatar_label_);

    lv_obj_t* dialogue_box = lv_obj_create(right_panel);
    lv_obj_set_size(dialogue_box, 164, 52);
    lv_obj_align(dialogue_box, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(dialogue_box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(dialogue_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dialogue_box, 1, 0);
    lv_obj_set_style_border_color(dialogue_box, lv_color_black(), 0);
    lv_obj_set_style_radius(dialogue_box, 0, 0);
    lv_obj_set_style_pad_all(dialogue_box, 6, 0);
    lv_obj_set_style_layout(dialogue_box, LV_LAYOUT_NONE, 0);

    dashboard_message_label_ = lv_label_create(dialogue_box);
    lv_obj_set_width(dashboard_message_label_, 148);
    lv_obj_set_height(dashboard_message_label_, 40);
    lv_label_set_long_mode(dashboard_message_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(dashboard_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(dashboard_message_label_, text_font, 0);
    lv_obj_set_style_text_color(dashboard_message_label_, lv_color_black(), 0);
    lv_label_set_text(dashboard_message_label_, "");
    lv_obj_center(dashboard_message_label_);
}

void CustomLcdDisplay::SetStatus(const char* status) {
    LvglDisplay::SetStatus(status);

    if (!setup_ui_called_) {
        return;
    }

    time_t now = time(nullptr);
    struct tm tm_now = {};
    if (localtime_r(&now, &tm_now) == nullptr || tm_now.tm_year < 2025 - 1900) {
        return;
    }

    char time_text[8];
    char date_text[16];
    strftime(time_text, sizeof(time_text), "%H:%M", &tm_now);
    strftime(date_text, sizeof(date_text), "%Y-%m-%d", &tm_now);

    DisplayLockGuard lock(this);
    if (clock_time_label_ != nullptr) {
        lv_label_set_text(clock_time_label_, time_text);
    }
    if (clock_date_label_ != nullptr) {
        lv_label_set_text(clock_date_label_, date_text);
    }
}

void CustomLcdDisplay::SetEmotion(const char* emotion) {
    (void)emotion;
}

void CustomLcdDisplay::SetChatMessage(const char* role, const char* content) {
    (void)role;
    if (dashboard_message_label_ == nullptr || content == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    lv_label_set_text(dashboard_message_label_, content);
}

void CustomLcdDisplay::ClearChatMessages() {
    if (dashboard_message_label_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    lv_label_set_text(dashboard_message_label_, "");
}

void CustomLcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    (void)image;
}

void CustomLcdDisplay::SetDashboardClock(const char* time_text, const char* date_text) {
    DisplayLockGuard lock(this);
    if (clock_time_label_ != nullptr && time_text != nullptr) {
        lv_label_set_text(clock_time_label_, time_text);
    }
    if (clock_date_label_ != nullptr && date_text != nullptr) {
        lv_label_set_text(clock_date_label_, date_text);
    }
}

void CustomLcdDisplay::SetDashboardAgenda(const char* text) {
    if (agenda_label_ == nullptr || text == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    lv_label_set_text(agenda_label_, text);
}

void CustomLcdDisplay::SetDashboardToolStatus(const char* text) {
    if (tool_status_label_ == nullptr || text == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    lv_label_set_text(tool_status_label_, text);
}

void CustomLcdDisplay::SetDashboardDrawing(const uint8_t* bitmap, int width, int height) {
    (void)bitmap;
    (void)width;
    (void)height;
}

void CustomLcdDisplay::SetDashboardMessage(const char* text) {
    SetChatMessage("system", text);
}

namespace {
void UpdateCardFonts(lv_obj_t* root, const lv_font_t* font) {
    if (root == nullptr || font == nullptr) {
        return;
    }

    uint32_t child_count = lv_obj_get_child_cnt(root);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* child = lv_obj_get_child(root, i);
        if (child == nullptr) {
            continue;
        }

        if (lv_obj_get_class(child) == &lv_label_class) {
            lv_obj_set_style_text_font(child, font, 0);
        }

        UpdateCardFonts(child, font);
    }
}
}  // namespace

void CustomLcdDisplay::SetTheme(Theme* theme) {
    if (theme == nullptr) {
        return;
    }

    Display::SetTheme(theme);
    if (!setup_ui_called_) {
        return;
    }

    DisplayLockGuard lock(this);
    auto* lvgl_theme = static_cast<LvglTheme*>(theme);
    if (lvgl_theme == nullptr || lvgl_theme->text_font() == nullptr ||
        lvgl_theme->icon_font() == nullptr) {
        return;
    }

    const lv_font_t* text_font = lvgl_theme->text_font()->font();
    const lv_font_t* icon_font = lvgl_theme->icon_font()->font();
    const lv_font_t* avatar_font = icon_font;
    if (lvgl_theme->large_icon_font() != nullptr && lvgl_theme->large_icon_font()->font() != nullptr) {
        avatar_font = lvgl_theme->large_icon_font()->font();
    }
    const lv_font_t* status_icon_font = icon_font;
    if (lvgl_theme->large_icon_font() != nullptr && lvgl_theme->large_icon_font()->font() != nullptr &&
        text_font != nullptr && text_font->line_height >= 40) {
        status_icon_font = lvgl_theme->large_icon_font()->font();
    }
    if (text_font == nullptr) {
        text_font = &BUILTIN_TEXT_FONT;
    }
    if (status_icon_font == nullptr) {
        status_icon_font = icon_font != nullptr ? icon_font : &BUILTIN_ICON_FONT;
    }

    auto* screen = lv_screen_active();
    if (screen != nullptr) {
        lv_obj_set_style_text_font(screen, text_font, 0);
        lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    }
    if (dashboard_root_ != nullptr) {
        lv_obj_set_style_text_font(dashboard_root_, text_font, 0);
        lv_obj_set_style_text_color(dashboard_root_, lvgl_theme->text_color(), 0);
        UpdateCardFonts(dashboard_root_, text_font);
    }
    if (network_label_ != nullptr) {
        lv_obj_set_style_text_font(network_label_, status_icon_font, 0);
        lv_obj_set_style_text_color(network_label_, lv_color_black(), 0);
    }
    if (mute_label_ != nullptr) {
        lv_obj_set_style_text_font(mute_label_, status_icon_font, 0);
        lv_obj_set_style_text_color(mute_label_, lv_color_black(), 0);
    }
    if (battery_label_ != nullptr) {
        lv_obj_set_style_text_font(battery_label_, status_icon_font, 0);
        lv_obj_set_style_text_color(battery_label_, lv_color_black(), 0);
    }
    if (status_label_ != nullptr) {
        lv_obj_set_style_text_font(status_label_, text_font, 0);
        lv_obj_set_style_text_color(status_label_, lv_color_black(), 0);
    }
    if (notification_label_ != nullptr) {
        lv_obj_set_style_text_font(notification_label_, text_font, 0);
        lv_obj_set_style_text_color(notification_label_, lv_color_black(), 0);
    }
    if (dashboard_message_label_ != nullptr) {
        lv_obj_set_style_text_font(dashboard_message_label_, text_font, 0);
        lv_obj_set_style_text_color(dashboard_message_label_, lv_color_black(), 0);
    }
    if (avatar_label_ != nullptr) {
        lv_obj_set_style_text_font(avatar_label_, avatar_font, 0);
        lv_obj_set_style_text_color(avatar_label_, lv_color_black(), 0);
    }
}
