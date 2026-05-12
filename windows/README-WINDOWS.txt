UG6 Broadcaster - bundle Windows portable
==========================================

Pour installer :

1. Installe une fois pour toutes Visual C++ 2015-2022 Redistributable (x64) :
   https://aka.ms/vs/17/release/vc_redist.x64.exe
   (clic, suivant, suivant, c'est fini)

2. Dezippe ce dossier ou tu veux (ex C:\UG6\).

3. Double-clique UG6Broadcaster.exe.

4. Ouvre http://127.0.0.1:8000 dans Chrome ou Edge.


Pour 10 radios sur la meme machine, lance plusieurs instances :

   UG6Broadcaster.exe --port=8000
   UG6Broadcaster.exe --port=8001
   UG6Broadcaster.exe --port=8002
   ... jusqu'a --port=8009

Chaque instance ouvre une console (laisse-la tourner) et garde sa propre
config Icecast dans :

   %APPDATA%\UG6Broadcaster\instances\config-port-XXXX.json


Astuces :
- Pour stopper une instance : ferme sa fenetre console ou Ctrl+C dedans.
- Pour les lancer en service au demarrage Windows : place les raccourcis
  dans shell:startup (taper dans le menu Demarrer).
- Les presets DSP perso sont dans %APPDATA%\UG6Broadcaster\presets\
