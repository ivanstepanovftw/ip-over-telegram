# love5an_bot

## Installation

1. Install Python 3.8 or higher
2. Install requirements
```shell
python -m pip install -r requirements.txt
```
3. Create `love5an_bot__secrets.py` file in the root directory
  Example of `/love5an_bot__secrets.py`:
  ```python
  userbot = {
      "files_directory": "UserbotDB",
      "token": "+15555555555",
      "api_id": 123456,
      "api_hash": "1234567890abcdef1234567890abcdef",
      "database_encryption_key": "userbot database encryption key"
  }
  
  bot = {
      "files_directory": "BotDB",
      "token": "1234567890:ABCDEFghijklmnopqrstuvwxyz123456",
      "api_id": 123456,
      "api_hash": "1234567890abcdef1234567890abcdef",
      "database_encryption_key": "bot database encryption key"
  }
  ```
