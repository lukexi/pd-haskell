module Sound.Pd1 (
    makePatch,
    makeWeakPatch,
    withPatch,
    closePatch,
    send,
    sendGlobal,
    subscribe,
    makeReceiveChan,
    Pd.local,
    Pd.Atom(..),
    Pd.Message(..)
    ) where
import qualified Sound.Pd as Pd

import Control.Concurrent
import System.IO.Unsafe
import Data.IORef

{-# NOINLINE pdRef #-}
pdRef :: IORef Pd.PureData
pdRef = unsafePerformIO $ newIORef =<< Pd.initLibPd

getPd :: IO Pd.PureData
getPd = readIORef pdRef

makePatch :: FilePath -> IO Pd.Patch
makePatch fileName = do
    pd <- getPd
    Pd.makePatch pd fileName

withPatch :: FilePath -> (Pd.Patch -> IO a) -> IO a
withPatch fileName action = do
    pd <- getPd
    Pd.withPatch pd fileName action

makeWeakPatch :: FilePath -> IO Pd.Patch
makeWeakPatch fileName = do
    pd <- getPd
    Pd.makeWeakPatch pd fileName

closePatch :: Pd.Patch -> IO ()
closePatch patch = do
    pd <- getPd
    Pd.closePatch pd patch

send :: Pd.Patch -> Pd.Receiver -> Pd.Message -> IO ()
send patch receiver msg = do
    pd <- getPd
    Pd.send pd patch receiver msg

makeReceiveChan name = do
    pd <- getPd
    Pd.makeReceiveChan pd name

-- | Sends a message to the given global receiver name
sendGlobal :: Pd.Receiver -> Pd.Message -> IO ()
sendGlobal r msg = do
    pd <- getPd
    Pd.sendGlobal pd r msg
 
subscribe :: Pd.Receiver -> (Pd.Message -> IO a) -> IO ThreadId
subscribe name handler = do
    pd <- getPd
    Pd.subscribe pd name handler