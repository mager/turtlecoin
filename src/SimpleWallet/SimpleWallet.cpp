/*
Copyright (C) 2018, The TurtleCoin developers

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include <SimpleWallet/SimpleWallet.h>

int main(int argc, char **argv)
{
    Config config = parseArguments(argc, argv);

    /* User requested --help or --version */
    if (config.exit)
    {
        return 0;
    }

    /* Only log to file so we don't have crap filling up the terminal */
    Logging::LoggerManager logManager;
    /* Should we maybe change this to INFO? On the one hand, DEBUGGING really
       spams the file, on the other hand, it exposes a lot of information
       that might help more devs debug a users issues. */
    logManager.setMaxLevel(Logging::DEBUGGING);

    Logging::FileLogger fileLogger;
    fileLogger.init("simplewallet.log");
    logManager.addLogger(fileLogger);

    Logging::LoggerRef logger(logManager, "simplewallet");

    /* Currency contains our coin parameters, such as decimal places, supply */
    CryptoNote::Currency currency 
        = CryptoNote::CurrencyBuilder(logManager).currency();

    System::Dispatcher localDispatcher;
    System::Dispatcher *dispatcher = &localDispatcher;

    /* Our connection to turtlecoind */
    std::unique_ptr<CryptoNote::INode> node(
        new CryptoNote::NodeRpcProxy(config.host, config.port, 
                                     logger.getLogger()));

    std::promise<std::error_code> errorPromise;
    std::future<std::error_code> error = errorPromise.get_future();
    auto callback = [&errorPromise](std::error_code e) 
                    {errorPromise.set_value(e); };

    node->init(callback);

    if (error.get())
    {
        throw("Failed to initialize node!");
    }

    /* Create the wallet instance */
    CryptoNote::WalletGreen wallet(*dispatcher, currency, *node, 
                                   logger.getLogger());

    /* Run the interactive wallet interface */
    run(wallet, *node);
}

void run(CryptoNote::WalletGreen &wallet, CryptoNote::INode &node)
{
    std::cout << InformationMsg("TurtleCoin v" + std::string(PROJECT_VERSION)
                              + " Simplewallet") << std::endl;

    /* Open/import/generate the wallet */
    Action action = getAction();
    std::shared_ptr<WalletInfo> walletInfo = handleAction(wallet, action);

    ThreadHandler threadHandler;

    /* This will call shutdown when ctrl+c is hit. This is a lambda function.
       We use &variable to capture a variable, allowing us to use it in the
       lambda. */
    Tools::SignalHandler::install([&threadHandler, &walletInfo, &node] {
        shutdown(walletInfo->wallet, node, threadHandler);
        /* You shouldn't really do this. I'm lazy and stupid. */
        exit(0);
    });

    if (node.getLastKnownBlockHeight() == 0)
    {
        std::cout << WarningMsg("It looks like TurtleCoind isn't open!")
                  << std::endl << std::endl
                  << WarningMsg("Ensure TurtleCoind is open and has finished "
                                "initializing.")
                  << std::endl
                  << WarningMsg("If it's still not working, try restarting "
                                "TurtleCoind. The daemon sometimes gets stuck.") 
                  << std::endl
                  << WarningMsg("Alternatively, perhaps TurtleCoind can't "
                                "communicate with any peers.")
                  << std::endl << std::endl
                  << WarningMsg("The wallet can't function until it can "
                                "communicate with the network.")
                  << std::endl
                  << InformationMsg("Hit any key to exit: ");

        std::cin.get();

        shutdown(walletInfo->wallet, node, threadHandler);
    }
    else
    {
        /* Scan the chain for new transactions. In the case of an imported 
           wallet, we need to scan the whole chain to find any transactions. 
           If we opened the wallet however, we just need to scan from when we 
           last had it open. If we are generating a wallet, there is no need
           to check for transactions as there is no way the wallet can have
           received any money yet. */
        if (action != Generate)
        {
            findNewTransactions(node, walletInfo->wallet);
        }
        else
        {
            std::cout << InformationMsg("Your wallet is syncing with the "
                                        "network in the background.")
                      << std::endl
                      << InformationMsg("Until this is completed new "
                                        "transactions might not show up.")
                      << std::endl
                      << InformationMsg("Use bc_height to check the progress.")
                      << std::endl << std::endl;
        }

        /* Look for transactions in the background */
        /* We need to use std::ref here, because when you pass a variable to
           a boost::thread, or a std::thread as well, they get copied by
           value, and then passed as reference to the function, so whilst it
           appears to have been passed by reference, it hasn't really. Using
           a std::ref wrapper fixes this. */
        boost::thread txWatcher(&transactionWatcher, walletInfo,
                                std::ref(threadHandler));

        welcomeMsg();

        inputLoop(walletInfo, node, threadHandler);

        shutdown(walletInfo->wallet, node, threadHandler);

        /* Wait for the tx watcher to terminate */
        txWatcher.join();
    }
}

std::shared_ptr<WalletInfo> handleAction(CryptoNote::WalletGreen &wallet,
                                         Action action)
{
    if (action == Generate)
    {
        return generateWallet(wallet);
    }
    else if (action == Open)
    {
        return openWallet(wallet);
    }
    else if (action == Import)
    {
        return importWallet(wallet);
    }
    else if (action == SeedImport)
    {
        return mnemonicImportWallet(wallet);
    }
    else if (action == ViewWallet)
    {
        return createViewWallet(wallet);
    }
    else
    {
        throw std::runtime_error("Unimplemented action!");
    }
}

std::shared_ptr<WalletInfo> createViewWallet(CryptoNote::WalletGreen &wallet)
{
    Crypto::SecretKey privateViewKey = getPrivateKey("Private View Key: ");

    CryptoNote::AccountPublicAddress publicKeys;
    uint64_t prefix;

    std::string address;

    while (true)
    {
        std::cout << "Public TRTL address: ";

        std::getline(std::cin, address);
        boost::algorithm::trim(address);

        if (address.length() != 99)
        {
            std::cout << WarningMsg("Address is wrong length!") << std::endl
                  << "It should be 99 characters long, but it is "
                  << address.length() << " characters long!" << std::endl;
        }
        else if (address.substr(0, 4) != "TRTL")
        {
            std::cout << WarningMsg("Invalid address! It should start with "
                                    "\"TRTL\"!") << std::endl;
        }
        else if (!CryptoNote::parseAccountAddressString(prefix, publicKeys,
                                                        address))
        {
            std::cout << WarningMsg("Failed to parse TRTL address! Ensure you "
                                    "have entered it correctly.")
                      << std::endl;
        }
        else
        {
            break;
        }
    }

    std::string walletFileName = getNewWalletFileName();
    std::string walletPass = getWalletPassword(true);

    wallet.createViewWallet(walletFileName, walletPass, address,
                            privateViewKey);

    std::cout << InformationMsg("\nYour view wallet " + address 
                              + " has been successfully imported!")
              << std::endl << std::endl;

    viewWalletMsg();

    return std::make_shared<WalletInfo>(walletFileName, walletPass, 
                                        address, true, wallet);
}

std::shared_ptr<WalletInfo> importWallet(CryptoNote::WalletGreen &wallet)
{
    Crypto::SecretKey privateSpendKey = getPrivateKey("Private Spend Key: ");
    Crypto::SecretKey privateViewKey = getPrivateKey("Private View Key: ");
    return importFromKeys(wallet, privateSpendKey, privateViewKey);
}

std::shared_ptr<WalletInfo> mnemonicImportWallet(CryptoNote::WalletGreen
                                                 &wallet)
{
    std::string mnemonicPhrase;

    Crypto::SecretKey privateSpendKey;
    Crypto::SecretKey privateViewKey;

    do
    {
        std::cout << "Mnemonic Phrase (25 words): ";
        std::getline(std::cin, mnemonicPhrase);
        boost::algorithm::trim(mnemonicPhrase);
    }
    while (!crypto::ElectrumWords::is_valid_mnemonic(mnemonicPhrase,
                                                     privateSpendKey));

    CryptoNote::AccountBase::generateViewFromSpend(privateSpendKey, 
                                                   privateViewKey);

    return importFromKeys(wallet, privateSpendKey, privateViewKey);
}

std::shared_ptr<WalletInfo> importFromKeys(CryptoNote::WalletGreen &wallet,
                                           Crypto::SecretKey privateSpendKey, 
                                           Crypto::SecretKey privateViewKey)
{
    std::string walletFileName = getNewWalletFileName();
    std::string walletPass = getWalletPassword(true);

    connectingMsg();

    wallet.initializeWithViewKey(walletFileName, walletPass, privateViewKey);

    std::string walletAddress = wallet.createAddress(privateSpendKey);

    std::cout << InformationMsg("\nYour wallet " + walletAddress 
                              + " has been successfully imported!")
              << std::endl << std::endl;

    return std::make_shared<WalletInfo>(walletFileName, walletPass, 
                                        walletAddress, false, wallet);
}

std::shared_ptr<WalletInfo> generateWallet(CryptoNote::WalletGreen &wallet)
{
    std::string walletFileName = getNewWalletFileName();
    std::string walletPass = getWalletPassword(true);

    
    CryptoNote::KeyPair spendKey;
    Crypto::SecretKey privateViewKey;

    Crypto::generate_keys(spendKey.publicKey, spendKey.secretKey);
    CryptoNote::AccountBase::generateViewFromSpend(spendKey.secretKey,
                                                   privateViewKey);

    wallet.initializeWithViewKey(walletFileName, walletPass, privateViewKey);

    std::string walletAddress = wallet.createAddress(spendKey.secretKey);

    promptSaveKeys(wallet);

    std::cout << WarningMsg("If you lose these your wallet cannot be recreated!")
              << std::endl << std::endl;

    return std::make_shared<WalletInfo>(walletFileName, walletPass,
                                        walletAddress, false, wallet);
}

std::shared_ptr<WalletInfo> openWallet(CryptoNote::WalletGreen &wallet)
{
    std::string walletFileName = getExistingWalletFileName();

    while (true)
    {
        std::string walletPass = getWalletPassword(false);

        connectingMsg();

        try
        {
            wallet.load(walletFileName, walletPass);

            std::string walletAddress = wallet.getAddress(0);
            
            Crypto::SecretKey privateSpendKey
                = wallet.getAddressSpendKey(0).secretKey;

            if (privateSpendKey == CryptoNote::NULL_SECRET_KEY)
            {
                std::cout << std::endl
                          << InformationMsg("Your view only wallet "
                                          + walletAddress
                                          + " has been successfully opened!")
                          << std::endl << std::endl;

                viewWalletMsg();

                return std::make_shared<WalletInfo>(walletFileName, walletPass, 
                                                    walletAddress, true, 
                                                    wallet);
            }
            else
            {
                std::cout << std::endl
                          << InformationMsg("Your wallet "
                                          + walletAddress
                                          + " has been successfully opened!")
                          << std::endl << std::endl;

                return std::make_shared<WalletInfo>(walletFileName, walletPass, 
                                                    walletAddress, false, 
                                                    wallet);

            }

            return std::make_shared<WalletInfo>(walletFileName, walletPass, 
                                                walletAddress, false, wallet);
        }
        catch (const std::system_error& e)
        {
            std::string walletSuccessBadPwdMsg = 
                "Restored view public key doesn't correspond to secret key: "
                "The password is wrong";

            std::string walletSuccessBadPwdMsg2 =
                "Restored spend public key doesn't correspond to secret key: "
                "The password is wrong";

            std::string walletLegacyBadPwdMsg =
                ": The password is wrong";

            std::string errorMsg = e.what();
                
            /* There are three different error messages depending upon if we're
               opening a walletgreen or a walletlegacy wallet */
            if (errorMsg == walletSuccessBadPwdMsg || 
                errorMsg == walletSuccessBadPwdMsg2 ||
                errorMsg == walletLegacyBadPwdMsg)
            {
                std::cout << WarningMsg("Incorrect password! Try again.")
                          << std::endl;
            }
            else
            {
                throw(e);
            }
        }
    }
}

Crypto::SecretKey getPrivateKey(std::string msg)
{
    size_t privateKeyLen = 64;
    size_t size;

    std::string privateKeyString;
    Crypto::Hash privateKeyHash;
    Crypto::SecretKey privateKey;
    Crypto::PublicKey publicKey;

    while (true)
    {
        std::cout << msg;

        std::getline(std::cin, privateKeyString);
        boost::algorithm::trim(privateKeyString);

        if (privateKeyString.length() != privateKeyLen)
        {
            std::cout << WarningMsg("Invalid private key, should be 64 "
                                    "characters! Try again.") << std::endl;
            continue;
        }
        else if (!Common::fromHex(privateKeyString, &privateKeyHash, 
                  sizeof(privateKeyHash), size)
                 || size != sizeof(privateKeyHash))
        {
            std::cout << WarningMsg("Invalid private key, failed to parse! "
                                    "Ensure you entered it correctly.")
                      << std::endl;
            continue;
        }

        privateKey = *(struct Crypto::SecretKey *) &privateKeyHash;

        /* Just used for verification purposes before we pass it to
           walletgreen */
        if (!Crypto::secret_key_to_public_key(privateKey, publicKey))
        {
            std::cout << "Invalid private key, failed to parse! Ensure "
                         "you entered it correctly." << std::endl;
            continue;
        }

        return privateKey;
    }
}

std::string getExistingWalletFileName()
{
    std::string walletName;

    while (true)
    {
        std::cout << "What is the name of the wallet you want to open?: ";
        std::getline(std::cin, walletName);

        std::string walletFileName = walletName + ".wallet";

        if (walletName == "")
        {
            std::cout << WarningMsg("Wallet name can't be blank! Try again.")
                      << std::endl;
        }
        /* Allow people to enter wallet name with or without file extension */
        else if (boost::filesystem::exists(walletName))
        {
            return walletName;
        }
        else if (boost::filesystem::exists(walletFileName))
        {
            return walletFileName;
        }
        else
        {
            std::cout << WarningMsg("A wallet with the filename " 
                                  + walletFileName + " doesn't exist!")
                      << std::endl
                      << "Ensure you entered your wallet name correctly."
                      << std::endl;
        }
    }
}

std::string getNewWalletFileName()
{
    std::string walletName;

    while (true)
    {
        std::cout << "What would you like to call your new wallet?: ";
        std::getline(std::cin, walletName);

        std::string walletFileName = walletName + ".wallet";

        if (boost::filesystem::exists(walletFileName))
        {
            std::cout << WarningMsg("A wallet with the filename " 
                                  + walletFileName + " already exists!")
                      << std::endl
                      << "Try another name." << std::endl;
        }
        else if (walletName == "")
        {
            std::cout << WarningMsg("Wallet name can't be blank! Try again.")
                      << std::endl;
        }
        else
        {
            return walletFileName;
        }
    }
}

std::string getWalletPassword(bool verifyPwd)
{
    Tools::PasswordContainer pwdContainer;
    pwdContainer.read_password(verifyPwd);
    return pwdContainer.password();
}

Action getAction()
{
    while (true)
    {
        std::cout << std::endl << "Welcome, please choose an option below:"
                  << std::endl << std::endl
                  
                  << "\t[" << InformationMsg("G") << "] - "
                  << "Generate a new wallet address"
                  << std::endl 

                  << "\t[" << InformationMsg("O") << "] - "
                  << "Open a wallet already on your system"
                  << std::endl
                  
                  << "\t[" << InformationMsg("S") << "] - "
                  << "Regenerate your wallet using a seed phrase of words"
                  << std::endl
                  
                  << "\t[" << InformationMsg("I") << "] - "
                  << "Import your wallet using a View Key and Spend Key"
                  << std::endl

                  << "\t[" << InformationMsg("V") << "] - "
                  << "Import a view only wallet (Unable to send transactions)"
                  << std::endl << std::endl

                  << "or, press CTRL_C to exit: ";

        std::string answer;
        std::getline(std::cin, answer);

        char c = answer[0];
        c = std::tolower(c);

        if (c == 'o')
        {
            return Open;
        }
        else if (c == 'g')
        {
            return Generate;
        }
        else if (c == 'i')
        {
            return Import;
        }
        else if (c == 's')
        {
            return SeedImport;
        }
        else if (c == 'v')
        {
            return ViewWallet;
        }
        else
        {
            std::cout << "Unknown command: " << WarningMsg(answer) << std::endl;
        }
    }
}

void promptSaveKeys(CryptoNote::WalletGreen &wallet)
{
    std::cout << "Welcome to your new wallet, here is your payment address:"
              << std::endl << InformationMsg(wallet.getAddress(0))
              << std::endl << std::endl 
              << "Please copy your secret keys and mnemonic seed and store "
              << "them in a secure location: " << std::endl;

    printPrivateKeys(wallet, false);

    std::cout << std::endl;
}

void exportKeys(std::shared_ptr<WalletInfo> walletInfo)
{
    confirmPassword(walletInfo->walletPass);
    printPrivateKeys(walletInfo->wallet, walletInfo->viewWallet);
}

void printPrivateKeys(CryptoNote::WalletGreen &wallet, bool viewWallet)
{
    Crypto::SecretKey privateViewKey = wallet.getViewKey().secretKey;

    if (viewWallet)
    {
        std::cout << SuccessMsg("Private view key: " 
                              + Common::podToHex(privateViewKey)) << std::endl;
        return;
    }

    Crypto::SecretKey privateSpendKey = wallet.getAddressSpendKey(0).secretKey;

    Crypto::SecretKey derivedPrivateViewKey;

    CryptoNote::AccountBase::generateViewFromSpend(privateSpendKey,
                                                   derivedPrivateViewKey);

    bool deterministicPrivateKeys = derivedPrivateViewKey == privateViewKey;

    std::cout << SuccessMsg("Private spend key: " 
                          + Common::podToHex(privateSpendKey)) << std::endl
              << SuccessMsg("Private view key: " 
                          + Common::podToHex(privateViewKey)) << std::endl;

    if (deterministicPrivateKeys)
    {
        std::string mnemonicSeed;

        crypto::ElectrumWords::bytes_to_words(privateSpendKey, 
                                              mnemonicSeed,
                                              "English");

        std::cout << SuccessMsg("Mnemonic seed: " + mnemonicSeed) << std::endl;
    }
}

void welcomeMsg()
{
    std::cout << "Use the " << SuggestionMsg("help") 
              << " command to see the list "
              << "of available commands." << std::endl << "Use "
              << SuggestionMsg("exit") << " when closing to ensure your wallet "
              << "file doesn't get corrupted." << std::endl << std::endl;
}

void inputLoop(std::shared_ptr<WalletInfo> walletInfo, CryptoNote::INode &node,
               ThreadHandler &threadHandler)
{ 
    while (true)
    {
        std::cout << getPrompt(walletInfo);

        std::string command;
        std::getline(std::cin, command);

        /* Split into args to support legacy transfer command, for example
           transfer 5 TRTLxyz... 100, sends 100 TRTL to TRTLxyz... with a mixin
           of 5 */
        std::vector<std::string> words;
        words = boost::split(words, command, ::isspace);
        
        if (command == "")
        {
            // no-op
        }
        else if (command == "export_keys")
        {
            exportKeys(walletInfo);
        }
        else if (command == "help")
        {
            help(walletInfo->viewWallet);
        }
        else if (command == "balance")
        {
            balance(node, walletInfo->wallet, walletInfo->viewWallet);
        }
        else if (command == "address")
        {
            std::cout << SuccessMsg(walletInfo->walletAddress) << std::endl;
        }
        else if (command == "incoming_transfers")
        {
            listTransfers(true, false, walletInfo->wallet);
        }
        else if (command == "exit")
        {
            return;
        }
        else if (command == "bc_height")
        {
            blockchainHeight(node, walletInfo->wallet);
        }
        else if (command == "reset")
        {
            reset(node, walletInfo, threadHandler);
        }
        else if (!walletInfo->viewWallet)
        {
            if (command == "outgoing_transfers")
            {
                listTransfers(false, true, walletInfo->wallet);
            }
            else if (command == "list_transfers")
            {
                listTransfers(true, true, walletInfo->wallet);
            }
            else if (command == "transfer")
            {
                transfer(walletInfo);
            }
            else if (words[0] == "transfer")
            {
                /* remove the first item from words - this is the "transfer"
                   command, leaving us just the transfer arguments. */
                words.erase(words.begin());
                transfer(walletInfo, words);
            }
            else if (command == "quick_optimize")
            {
                quickOptimize(walletInfo->wallet);
            }
            else if (command == "full_optimize")
            {
                fullOptimize(walletInfo->wallet);
            }
            else
            {
                std::cout << "Unknown command: " << WarningMsg(command) 
                          << ", use " << SuggestionMsg("help") 
                          << " command to list all possible commands."
                          << std::endl;
            }
        }
        else
        {
            std::cout << "Unknown command: " << WarningMsg(command) 
                      << ", use " << SuggestionMsg("help") 
                      << " command to list all possible commands." << std::endl
                      << "Please note some commands such as transfer are "
                      << "unavailable, as you are using a view only wallet."
                      << std::endl;
        }
    }
}

void help(bool viewWallet)
{
    std::cout << "Available commands:" << std::endl
              << SuccessMsg("help", 25)
              << "List this help message" << std::endl
              << SuccessMsg("reset", 25)
              << "Discard cached data and recheck for transactions" << std::endl
              << SuccessMsg("bc_height", 25)
              << "Show the blockchain height" << std::endl
              << SuccessMsg("balance", 25)
              << "Display how much TRTL you have" << std::endl
              << SuccessMsg("export_keys", 25)
              << "Export your private keys" << std::endl
              << SuccessMsg("address", 25)
              << "Displays your payment address" << std::endl
              << SuccessMsg("exit", 25)
              << "Exit and save your wallet" << std::endl
              << SuccessMsg("incoming_transfers", 25)
              << "Show incoming transfers" << std::endl;
                  
    if (viewWallet)
    {
        std::cout << InformationMsg("Please note you are using a view only "
                                    "wallet, and so cannot transfer TRTL.")
                  << std::endl;
    }
    else
    {
        std::cout << SuccessMsg("outgoing_transfers", 25)
                  << "Show outgoing transfers" << std::endl
                  << SuccessMsg("list_transfers", 25)
                  << "Show all transfers" << std::endl
                  << SuccessMsg("quick_optimize", 25)
                  << "Quickly optimize your wallet to send large amounts"
                  << std::endl
                  << SuccessMsg("full_optimize", 25)
                  << "Fully optimize your wallet to send large amounts"
                  << std::endl
                  << SuccessMsg("transfer", 25)
                  << "Send TRTL to someone" << std::endl;
    }
}

void balance(CryptoNote::INode &node, CryptoNote::WalletGreen &wallet,
             bool viewWallet)
{
    uint64_t unconfirmedBalance = wallet.getPendingBalance();
    uint64_t confirmedBalance = wallet.getActualBalance();
    uint64_t totalBalance = unconfirmedBalance + confirmedBalance;

    uint32_t localHeight = node.getLastLocalBlockHeight();
    uint32_t remoteHeight = node.getLastKnownBlockHeight();
    uint32_t walletHeight = wallet.getBlockCount();

    std::cout << "Available balance: "
              << SuccessMsg(formatAmount(confirmedBalance)) << std::endl
              << "Locked (unconfirmed) balance: "
              << WarningMsg(formatAmount(unconfirmedBalance))
              << std::endl << "Total balance: "
              << InformationMsg(formatAmount(totalBalance)) << std::endl;

    if (viewWallet)
    {
        std::cout << InformationMsg("Please note that view only wallets "
                                    "can only track incoming transactions, "
                                    "and so your wallet balance may appear "
                                    "inflated.") << std::endl;
    }

    if (localHeight < remoteHeight)
    {
        std::cout << InformationMsg("Your daemon is not fully synced with "
                                    "the network!")
                  << std::endl << "Your balance may be incorrect until you "
                  << "are fully synced!" << std::endl << std::endl;
    }
    /* Small buffer because wallet height doesn't update instantly like node
       height does */
    else if (walletHeight + 1000 < remoteHeight)
    {
        std::cout << InformationMsg("The blockchain is still being scanned for "
                                    "your transactions.")
                  << std::endl
                  << "Balances might be incorrect whilst this is ongoing."
                  << std::endl;
    }
}

void blockchainHeight(CryptoNote::INode &node, CryptoNote::WalletGreen &wallet)
{
    uint32_t localHeight = node.getLastLocalBlockHeight();
    uint32_t remoteHeight = node.getLastKnownBlockHeight();
    uint32_t walletHeight = wallet.getBlockCount();

    /* This is the height that the wallet has been scanned to. The blockchain
       can be fully updated, but we have to walk the chain to find our
       transactions, and this number indicates that progress. */
    std::cout << "Wallet blockchain height: ";

    /* Small buffer because wallet height doesn't update instantly like node
       height does */
    if (walletHeight + 1000 > remoteHeight)
    {
        std::cout << SuccessMsg(std::to_string(walletHeight));
    }
    else
    {
        std::cout << WarningMsg(std::to_string(walletHeight));
    }

    std::cout << std::endl << "Local blockchain height: ";

    if (localHeight == remoteHeight)
    {
        std::cout << SuccessMsg(std::to_string(localHeight));
    }
    else
    {
        std::cout << WarningMsg(std::to_string(localHeight));
    }

    std::cout << std::endl << "Network blockchain height: "
              << SuccessMsg(std::to_string(remoteHeight)) << std::endl;

    if (localHeight == 0 && remoteHeight == 0)
    {
        std::cout << WarningMsg("Uh oh, it looks like you don't have "
                                "TurtleCoind open!")
                  << std::endl;
    }
    else if (walletHeight + 1000 < remoteHeight && localHeight == remoteHeight)
    {
        std::cout << InformationMsg("You are synced with the network, but the "
                                    "blockchain is still being scanned for "
                                    "your transactions.")
                  << std::endl
                  << "Balances might be incorrect whilst this is ongoing."
                  << std::endl;
    }
    else if (localHeight == remoteHeight)
    {
        std::cout << SuccessMsg("Yay! You are synced!") << std::endl;
    }
    else
    {
        std::cout << WarningMsg("Be patient, you are still syncing with the "
                                "network!") << std::endl;
    }
}

void shutdown(CryptoNote::WalletGreen &wallet, CryptoNote::INode &node,
              ThreadHandler &threadHandler)
{
    if (threadHandler.shouldDie)
    {
        std::cout << "Patience little turtle, we're already shutting down!" 
                  << std::endl;
        return;
    }
    else
    {
        std::cout << InformationMsg("Saving wallet and shutting down, please "
                                    "wait...") << std::endl;
    }

    threadHandler.shouldDie = true;

    /* This is a lambda function. We have to "capture" wallet and node so they
       can be used inside our lambda. */
    boost::thread shutdownThread([&wallet, &node]()
    {
            wallet.save();
            wallet.shutdown();
            node.shutdown();
    });

    bool shutdownSuccess 
       = shutdownThread.try_join_for(boost::chrono::seconds(20));

    /* If we fail to shutdown correctly, then force close. Otherwise when
       we go out of main scope we'll get an ugly error message when terminate
       is called on the node/wallet thread */
    if (!shutdownSuccess)
    {
        std::cout << WarningMsg("Wallet took too long to save! Force closing.") 
                  << std::endl;
        std::cout << "Bye." << std::endl;
        exit(0);
    }

    std::cout << "Bye." << std::endl;
}

void printOutgoingTransfer(CryptoNote::WalletTransaction t)
{
    std::cout << WarningMsg("Outgoing transfer: " + Common::podToHex(t.hash) +
                            "\nSpent: " + formatAmount(-t.totalAmount - t.fee) + 
                            "\nFee: " + formatAmount(t.fee) +
                            "\nTotal Spent: " + formatAmount(-t.totalAmount)) 
              << std::endl << std::endl;
}

void printIncomingTransfer(CryptoNote::WalletTransaction t)
{
    std::cout << SuccessMsg("Incoming transfer: " + Common::podToHex(t.hash) +
                            "\nAmount: " + formatAmount(t.totalAmount))
              << std::endl << std::endl;
}

void listTransfers(bool incoming, bool outgoing, 
                   CryptoNote::WalletGreen &wallet)
{
    size_t numTransactions = wallet.getTransactionCount();
    int64_t totalSpent = 0;
    int64_t totalReceived = 0;

    for (size_t i = 0; i < numTransactions; i++)
    {
        CryptoNote::WalletTransaction t = wallet.getTransaction(i);

        if (t.totalAmount < 0 && outgoing)
        {
            printOutgoingTransfer(t);
            totalSpent += -t.totalAmount;
        }
        else if (t.totalAmount > 0 && incoming)
        {
            printIncomingTransfer(t);
            totalReceived += t.totalAmount;
        }
    }

    if (incoming)
    {
        std::cout << SuccessMsg("Total received: " 
                              + formatAmount(totalReceived))
                  << std::endl;
    }

    if (outgoing)
    {
        std::cout << WarningMsg("Total spent: " + formatAmount(totalSpent))
                  << std::endl;
    }
}

void transactionWatcher(std::shared_ptr<WalletInfo> walletInfo,
                        ThreadHandler &threadHandler)
{
    size_t transactionCount = walletInfo->wallet.getTransactionCount();

    while(true)
    {
        if (threadHandler.shouldDie)
        {
            return;
        }

        /* Pause watching until we get the signal to unpause. Will fuck up
           reset. */
        if (threadHandler.shouldPause)
        {
            threadHandler.havePaused = true;

            while (threadHandler.shouldPause)
            {
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
        }

        size_t tmpTransactionCount = walletInfo->wallet.getTransactionCount();

        if (tmpTransactionCount != transactionCount)
        {
            for (size_t i = transactionCount; i < tmpTransactionCount; i++)
            {
                CryptoNote::WalletTransaction t 
                    = walletInfo->wallet.getTransaction(i);

                /* Don't print outgoing or fusion transfers */
                if (t.totalAmount > 0)
                {
                    std::cout << std::endl
                              << InformationMsg("New transaction found!")
                              << std::endl
                              << SuccessMsg("Incoming transfer: " 
                                        + Common::podToHex(t.hash) +
                                          "\nAmount: " 
                                        + formatAmount(t.totalAmount))
                              << std::endl
                              << getPrompt(walletInfo)
                              << std::flush;
                }
            }

            transactionCount = tmpTransactionCount;
        }

        walletInfo->wallet.updateInternalCache();

        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

void reset(CryptoNote::INode &node, std::shared_ptr<WalletInfo> walletInfo,
           ThreadHandler &threadHandler)
{
    std::cout << InformationMsg("Resetting wallet...") << std::endl;

    /* Pause the transaction watcher whilst we reset. It could potentially
       fuck up, and if it doesn't, it still will be printing stuff when we
       only want findNewTransactions to be printing */
    threadHandler.shouldPause = true;

    while (!threadHandler.havePaused)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    /* Wallet is now unitialized. You must reinit with load, initWithKeys,
       or whatever. This function wipes the cache, then saves the wallet. */
    walletInfo->wallet.clearCacheAndShutdown();

    /* Now, we reopen the wallet. It now has no cached tx's, and balance */
    walletInfo->wallet.load(walletInfo->walletFileName,
                            walletInfo->walletPass);

    /* Now we rescan the chain to re-discover our balance and transactions */
    findNewTransactions(node, walletInfo->wallet);

    /* Then we save the wallet again, with the hopefully fixed balance. This
       step is not strictly necessary, but it's nice to do in case the user
       doesn't shut down cleanly. */
    walletInfo->wallet.save();

    threadHandler.shouldPause = false;
    threadHandler.shouldDie = false;
    threadHandler.havePaused = false;
}

void findNewTransactions(CryptoNote::INode &node, 
                         CryptoNote::WalletGreen &wallet)
{
    uint32_t localHeight = node.getLastLocalBlockHeight();
    uint32_t walletHeight = wallet.getBlockCount();
    uint32_t remoteHeight = node.getLastKnownBlockHeight();

    size_t transactionCount = wallet.getTransactionCount();

    int stuckCounter = 0;

    if (localHeight != remoteHeight)
    {
        std::cout << "Your TurtleCoind isn't fully synced yet!" << std::endl
                  << "Until you are fully synced, you won't be able to send "
                  << "transactions, and your balance may be missing or "
                  << "incorrect!" << std::endl << std::endl;
    }

    /* If we open a legacy wallet then it will load the transactions but not
       have the walletHeight == transaction height. Lets just throw away the
       transactions and rescan. */
    if (walletHeight == 1 && transactionCount != 0)
    {
        std::cout << "Upgrading your wallet from an older version of the "
                  << "software..." << std::endl << "Unfortunately, we have "
                  << "to rescan the chain to find your transactions."
                  << std::endl;
        transactionCount = 0;
        wallet.clearCaches(true, false);
    }

    if (walletHeight == 1)
    {
        std::cout << "Scanning through the blockchain to find transactions "
                     "that belong to you." << std::endl
                     << "Please wait, this will take some time."
                     << std::endl << std::endl;
    }
    else
    {
        std::cout << "Scanning through the blockchain to find any new "
                     "transactions you received whilst your wallet wasn't "
                     "open." << std::endl
                     << "Please wait, this may take some time."
                     << std::endl << std::endl;
    }

    while (walletHeight < localHeight)
    {
        wallet.updateInternalCache();

        size_t tmpTransactionCount = wallet.getTransactionCount();

        uint32_t tmpWalletHeight = wallet.getBlockCount();

        if (tmpWalletHeight == walletHeight)
        {
            stuckCounter++;
        }
        else
        {
            stuckCounter = 0;
        }

        /* Should be around a minute */
        if (stuckCounter > 60)
        {
            std::cout << WarningMsg("It looks like syncing might have got "
                                    "stuck...") << std::endl
                      << WarningMsg("This is probably due to TurtleCoind not "
                                    "responding. Try restarting TurtleCoind "
                                    "and the wallet.") << std::endl
                      << WarningMsg("If this error still continues after "
                                    "restarting the software, you may need to "
                                    "resync the blockchain.") << std::endl
                      << WarningMsg("See https://github.com/turtlecoin/"
                                    "turtlecoin/wiki/Bootstrapping-the-"
                                    "Blockchain for a quicker sync.")
                      << std::endl;

        }

        walletHeight = tmpWalletHeight;

        localHeight = node.getLastLocalBlockHeight();
        remoteHeight = node.getLastKnownBlockHeight();

        std::cout << SuccessMsg(std::to_string(walletHeight))
                  << " of " << InformationMsg(std::to_string(localHeight))
                  << std::endl;

        if (tmpTransactionCount != transactionCount)
        {
            for (size_t i = transactionCount; i < tmpTransactionCount; i++)
            {
                CryptoNote::WalletTransaction t = wallet.getTransaction(i);

                /* Don't print out fusion transactions */
                if (t.totalAmount != 0)
                {
                    std::cout << std::endl
                              << InformationMsg("New transaction found!")
                              << std::endl << std::endl;

                    if (t.totalAmount < 0)
                    {
                        printOutgoingTransfer(t);
                    }
                    else
                    {
                        printIncomingTransfer(t);
                    }
                }
            }

            transactionCount = tmpTransactionCount;
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << SuccessMsg("Finished scanning blockchain!") << std::endl
              << std::endl;

    /* In case the user force closes, we don't want them to have to rescan
       the whole chain. */
    wallet.save();
}

ColouredMsg getPrompt(std::shared_ptr<WalletInfo> walletInfo)
{
    const int promptLength = 20;
    const std::string extension = ".wallet";

    std::string walletName = walletInfo->walletFileName;

    /* Filename ends in .wallet, remove extension */
    if (std::equal(extension.rbegin(), extension.rend(), 
                   walletInfo->walletFileName.rbegin()))
    {
        int extPos = walletInfo->walletFileName.find_last_of('.');

        walletName = walletInfo->walletFileName.substr(0, extPos);
    }

    std::string shortName = walletName.substr(0, promptLength);

    return InformationMsg("[TRTL " + shortName + "]: ");
}

void connectingMsg()
{
    std::cout << std::endl << "Making initial contact with TurtleCoind."
              << std::endl
              << "Please wait, this sometimes can take a long time..."
              << std::endl << std::endl;
}

void viewWalletMsg()
{
    std::cout << InformationMsg("Please remember that when using a view wallet "
                                "you can only view incoming transactions!")
              << std::endl << "This means if you received 100 TRTL and then "
              << "sent 50 TRTL, your balance would appear to still be 100 "
              << "TRTL." << std::endl
              << "To effectively use a view wallet, you should only deposit "
              << "to this wallet." << std::endl
              << "If you have since needed to withdraw, send your remaining "
              << "balance to a new wallet, and import this as a new view "
              << "wallet so your balance can be correctly observed."
              << std::endl << std::endl;
}
