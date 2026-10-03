/* silent_mp3.S -- gtasa_nx's silent MP3, handed to the engine for a missing
 * radio track (it retries a failed .mp3 open forever otherwise). */
  .section .rodata
  .global silent_mp3
  .global silent_mp3_end
  .balign 16
silent_mp3:
  .incbin SILENT_MP3_PATH
silent_mp3_end:
  .byte 0
  .section .note.GNU-stack, "", %progbits
