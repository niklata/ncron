#!/bin/sh

touch test_history && ./ncron -V -t test_crontab -H test_history
