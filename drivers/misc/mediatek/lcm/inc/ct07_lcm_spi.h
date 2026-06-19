#ifndef __CT07_LCM_SPI_H__
#define __CT07_LCM_SPI_H__

int ct07_lcm_spi_send_cmd(unsigned int cmd);
int ct07_lcm_spi_send_data(const unsigned char *data, unsigned int len);
void ct07_lcm_diag_stage(const char *stage);

#endif
