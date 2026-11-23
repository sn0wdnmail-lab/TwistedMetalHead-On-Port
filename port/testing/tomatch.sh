#!/bin/bash
# Boots the game with drive.sh and goes into a Challenge match on Big Blue Stadium (one opponent).
# The waits suit the software renderer on a slow machine; shorten them on a fast one.
D=$(dirname "$0")/drive.sh
$D start > /dev/null
sleep 50; $D key x 0.4 > /dev/null; sleep 4; $D key x 0.4 > /dev/null   # memory card notice
sleep 30; $D key x 0.4 > /dev/null                                       # game select: Head-On
sleep 35; $D key x 0.4 > /dev/null                                       # skip the intro movie
sleep 12; $D key x 0.4 > /dev/null                                       # Single Player
sleep 10; $D key Down 0.3 > /dev/null; sleep 3; $D key x 0.4 > /dev/null # Challenge
sleep 20; $D key x 0.4 > /dev/null                                       # vehicle
sleep 15; $D key x 0.4 > /dev/null                                       # battleground
sleep 15; $D key x 0.4 > /dev/null; sleep 6; $D key Down 0.3 > /dev/null; sleep 4; $D key x 0.4 > /dev/null # opponent, start
sleep 60; $D shot match > /dev/null; echo "in a match (see the 'match' screenshot)"
